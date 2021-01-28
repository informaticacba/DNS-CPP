/**
 *  Udp.cpp
 * 
 *  Implementation file for the Udp class
 * 
 *  @author Emiel Bruijntjes <emiel.bruijntjes@copernica.com>
 *  @copyright 2020 - 2021 Copernica BV
 */

/**
 *  Dependencies
 */
#include "../include/dnscpp/udp.h"
#include "../include/dnscpp/loop.h"
#include "../include/dnscpp/ip.h"
#include "../include/dnscpp/query.h"
#include "../include/dnscpp/core.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <stdexcept>
#include <unistd.h>
#include <poll.h>
#include "now.h"

/**
 *  Begin of namespace
 */
namespace DNS {

/**
 *  Constructor
 *  @param  core        core object
 *  @param  handler     object that is notified about incoming messages
 *  @throws std::runtime_error
 */
Udp::Udp(Core *core, Handler *handler) : 
    _core(core), 
    _handler(handler)
{
}

/**
 *  Destructor
 */
Udp::~Udp()
{
    // close the socket
    close();

    // stop monitoring the idle state
    stop();
}

/**
 *  Helper method to set a socket option
 *  @param  optname
 *  @param  optval
 *  @param  optlen
 */
int Udp::setintopt(int optname, int32_t optval)
{
    // set the socket option
    return setsockopt(_fd, SOL_SOCKET, optname, &optval, 4);
}

/**
 *  Open the socket
 *  @param  version
 *  @return bool
 */
bool Udp::open(int version)
{
    // if already open
    if (_fd >= 0) return true;
    
    // try to open it (note that we do not set the NONBLOCK option, because we have not implemented 
    // buffering for the sendto() call (this could be a future optimization)
    _fd = socket(version == 6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    
    // check for success
    if (_fd < 0) return false;

    // if there is a buffer size to set, do so
    if (_core->buffersize() > 0)
    {
        // set the send and receive buffer to the requested buffer size
        setintopt(SO_SNDBUF, _core->buffersize());
        setintopt(SO_RCVBUF, _core->buffersize());
    }

    // we want to be notified when the socket receives data
    _identifier = _core->loop()->add(_fd, 1, this);
    
    // done
    return true;
}

/**
 *  Helper method to stop monitoring the idle state
 */
void Udp::stop()
{
    // nothing to do if not checking for idle
    if (_idle == nullptr) return;

    // cancel the idle watcher
    _core->loop()->cancel(_idle, this);

    // forget the ptr
    _idle = nullptr;
}

/**
 *  Close the socket
 *  @return bool
 */
bool Udp::close()
{
    // if already closed
    if (_fd < 0) return false;

    // tell the event loop that we no longer are interested in notifications
    _core->loop()->remove(_identifier, _fd, this);
    
    // close the socket
    ::close(_fd);
    
    // remember that socket is closed
    _fd = -1; _identifier = nullptr;
    
    // done
    return true;
}

/**
 *  Is the socket now readable?
 *  @return bool
 */
bool Udp::readable() const
{
    // if not active
    if (_fd < 0) return false;
    
    // structure required by the poll() call
    pollfd info;
    
    // fill the structure
    info.fd = _fd;
    info.events = POLLIN;
    info.revents = 0;
    
    // do the call
    return poll(&info, 1, 0) > 0;
}

/**
 *  Method that is called from user-space when the socket becomes readable.
 *  @param  now
 */
void Udp::notify()
{
    // do nothing if there is no socket (how is that possible!?)
    if (_fd < 0) return;
    
    // the buffer to receive the response in
    // @todo use a macro
    char buffer[65536];

    // structure will hold the source address (we use an ipv6 struct because that is also big enough for ipv4)
    struct sockaddr_in6 from; socklen_t fromlen = sizeof(from);
    
    // number of mesages gotten from the fd
    size_t messages = 0;

    // current time
    Now now;

    // we want to get as much messages at onces as possible
    // @todo use scatter-gather io to optimize this further
    do 
    {
        // reveive the message (the DONTWAIT option is needed because this is a blocking socket, but we dont want to block now)
        auto bytes = recvfrom(_fd, buffer, sizeof(buffer), MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
        
        // if there were no bytes, leap out
        if (bytes <= 0) break;

        // add to the responses
        _responses.emplace_back(now, (struct sockaddr *)&from, buffer, bytes);

    // we keep iterating as long as we've gotten under 1024 messages
    // @todo reduce this if too much time is taken doing this, and other buffers overflow
    } 
    while (++messages < 1024);

    // if we're already processing, we don't need to install idle watcher
    if (_idle) return;

    // create a new idle watcher now
    _idle = _core->loop()->idle(this);
}

/**
 *  Method that is called from user-space when the timer expires.
 */
void Udp::idle()
{
    // if there is nothing to do, we can stop the watcher (no more responses to feed back)
    if (_responses.empty()) return stop();

    // prevent exceptions (parsing the ip could fail)
    try
    {
        // note that the _handler->onReceived() method must be the LAST CALL in this function,
        // because after this call to userspace, "this" could very well have been destructed,
        // so we first have to remove the element from the list, before we call the handler,
        // to do this without copying, we create a list with just one element
        decltype(_responses) oneitem;
        
        // move the first item from the _responses to the one-item list
        oneitem.splice(oneitem.begin(), _responses, _responses.begin(), std::next(_responses.begin()));
        
        // get the first element
        const auto &front = oneitem.front();

        // get the first and only element from the list and pass to the handler (this must be the last
        // call in this function since the user-space handler cannot be trusted (it might destruct `this`))
        _handler->onReceived(front.ip(), front.data(), front.size());
    }
    catch (const std::runtime_error &error)
    {
        // @todo report the error
    }
}

/**
 *  Send a query to a nameserver (+open the socket when needed)
 *  @param  ip      IP address of the nameserver
 *  @param  query   the query to send
 *  @return bool
 */
bool Udp::send(const Ip &ip, const Query &query)
{
    // if the socket is not yet open we need to open it
    if (_fd < 0 && !open(ip.version())) return false;

    // should we bind in the ipv4 or ipv6 fashion?
    if (ip.version() == 6)
    {
        // structure to initialize
        struct sockaddr_in6 info;

        // fill the members
        info.sin6_family = AF_INET6;
        info.sin6_port = htons(53);
        info.sin6_flowinfo = 0;
        info.sin6_scope_id = 0;

        // copy the address
        memcpy(&info.sin6_addr, (const struct in6_addr *)ip, sizeof(struct in6_addr));
        
        // pass on to other method
        return send((struct sockaddr *)&info, sizeof(struct sockaddr_in6), query);
    }
    else
    {
        // structure to initialize
        struct sockaddr_in info;

        // fill the members
        info.sin_family = AF_INET;
        info.sin_port = htons(53);

        // copy address
        memcpy(&info.sin_addr, (const struct in_addr *)ip, sizeof(struct in_addr));

        // pass on to other method
        return send((const sockaddr *)&info, sizeof(struct sockaddr_in), query);
    }
}

/**
 *  Send a query to a certain nameserver
 *  @param  address     target address
 *  @param  size        size of the address
 *  @param  query       query to send
 *  @return bool
 */
bool Udp::send(const struct sockaddr *address, size_t size, const Query &query)
{
    // before we send we empty the buffer (this is an obscure optimization to deal
    // with use-cases where many domains are resolved, without any calls to the event
    // loop in between, the socket could then already hold some responses)
    if (readable()) notify();

    // send over the socket
    // @todo include MSG_DONTWAIT + implement non-blocking????
    return sendto(_fd, query.data(), query.size(), MSG_NOSIGNAL, address, size) >= 0;
}

/**
 *  End of namespace
 */
}
