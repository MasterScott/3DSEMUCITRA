// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "common/assert.h"
#include "common/bit_field.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"

#include "core/hle/kernel/session.h"
#include "core/hle/result.h"
#include "core/hle/service/soc_u.h"
#include "core/memory.h"

#ifdef _WIN64
    #include <winsock2.h>
    #include <ws2tcpip.h>

    // MinGW does not define several errno constants
    #ifndef _MSC_VER
        #define EBADMSG 104
        #define ENODATA 120
        #define ENOMSG  122
        #define ENOSR   124
        #define ENOSTR  125
        #define ETIME   137
        #define EIDRM   2001
        #define ENOLINK 2002
    #endif // _MSC_VER
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#ifdef _WIN64
#    define WSAEAGAIN      WSAEWOULDBLOCK
#    define WSAEMULTIHOP   -1 // Invalid dummy value
#    define ERRNO(x)       WSA##x
#    define GET_ERRNO      WSAGetLastError()
#    define poll(x, y, z)  WSAPoll(x, y, z);
#else
#    define ERRNO(x)       x
#    define GET_ERRNO      errno
#    define closesocket(x) close(x)
#endif

static const s32 SOCKET_ERROR_VALUE = -1;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace SOC_U

namespace SOC_U {

/// Holds the translation from system network errors to 3DS network errors
static const std::unordered_map<int, int> error_map = { {
    { E2BIG, 1 },
    { ERRNO(EACCES), 2 },
    { ERRNO(EADDRINUSE), 3 },
    { ERRNO(EADDRNOTAVAIL), 4 },
    { ERRNO(EAFNOSUPPORT), 5 },
    { ERRNO(EAGAIN), 6 },
    { ERRNO(EALREADY), 7 },
    { ERRNO(EBADF), 8 },
    { EBADMSG, 9 },
    { EBUSY, 10 },
    { ECANCELED, 11 },
    { ECHILD, 12 },
    { ERRNO(ECONNABORTED), 13 },
    { ERRNO(ECONNREFUSED), 14 },
    { ERRNO(ECONNRESET), 15 },
    { EDEADLK, 16 },
    { ERRNO(EDESTADDRREQ), 17 },
    { EDOM, 18 },
    { ERRNO(EDQUOT), 19 },
    { EEXIST, 20 },
    { ERRNO(EFAULT), 21 },
    { EFBIG, 22 },
    { ERRNO(EHOSTUNREACH), 23 },
    { EIDRM, 24 },
    { EILSEQ, 25 },
    { ERRNO(EINPROGRESS), 26 },
    { ERRNO(EINTR), 27 },
    { ERRNO(EINVAL), 28 },
    { EIO, 29 },
    { ERRNO(EISCONN), 30 },
    { EISDIR, 31 },
    { ERRNO(ELOOP), 32 },
    { ERRNO(EMFILE), 33 },
    { EMLINK, 34 },
    { ERRNO(EMSGSIZE), 35 },
    { ERRNO(EMULTIHOP), 36 },
    { ERRNO(ENAMETOOLONG), 37 },
    { ERRNO(ENETDOWN), 38 },
    { ERRNO(ENETRESET), 39 },
    { ERRNO(ENETUNREACH), 40 },
    { ENFILE, 41 },
    { ERRNO(ENOBUFS), 42 },
    { ENODATA, 43 },
    { ENODEV, 44 },
    { ENOENT, 45 },
    { ENOEXEC, 46 },
    { ENOLCK, 47 },
    { ENOLINK, 48 },
    { ENOMEM, 49 },
    { ENOMSG, 50 },
    { ERRNO(ENOPROTOOPT), 51 },
    { ENOSPC, 52 },
    { ENOSR, 53 },
    { ENOSTR, 54 },
    { ENOSYS, 55 },
    { ERRNO(ENOTCONN), 56 },
    { ENOTDIR, 57 },
    { ERRNO(ENOTEMPTY), 58 },
    { ERRNO(ENOTSOCK), 59 },
    { ENOTSUP, 60 },
    { ENOTTY, 61 },
    { ENXIO, 62 },
    { ERRNO(EOPNOTSUPP), 63 },
    { EOVERFLOW, 64 },
    { EPERM, 65 },
    { EPIPE, 66 },
    { EPROTO, 67 },
    { ERRNO(EPROTONOSUPPORT), 68 },
    { ERRNO(EPROTOTYPE), 69 },
    { ERANGE, 70 },
    { EROFS, 71 },
    { ESPIPE, 72 },
    { ESRCH, 73 },
    { ERRNO(ESTALE), 74 },
    { ETIME, 75 },
    { ERRNO(ETIMEDOUT), 76 }
}};

/// Converts a network error from platform-specific to 3ds-specific
static int TranslateError(int error) {
    auto found = error_map.find(error);
    if (found != error_map.end())
        return -found->second;

    return error;
}

/// Holds the translation from system network socket options to 3DS network socket options
/// Note: -1 = No effect/unavailable
static const std::unordered_map<int, int> sockopt_map = { {
    { 0x0004,   SO_REUSEADDR },
    { 0x0080,   -1 },
    { 0x0100,   -1 },
    { 0x1001,   SO_SNDBUF },
    { 0x1002,   SO_RCVBUF },
    { 0x1003,   -1 },
#ifdef _WIN64
    /// Unsupported in WinSock2
    { 0x1004,   -1 },
#else
    { 0x1004,   SO_RCVLOWAT },
#endif
    { 0x1008,   SO_TYPE },
    { 0x1009,   SO_ERROR },
}};

/// Converts a socket option from 3ds-specific to platform-specific
static int TranslateSockOpt(int console_opt_name) {
    auto found = sockopt_map.find(console_opt_name);
    if (found != sockopt_map.end()) {
        return found->second;
    }
    return console_opt_name;
}

/// Holds information about a particular socket
struct SocketHolder {
    u32 socket_fd; ///< The socket descriptor
    bool blocking; ///< Whether the socket is blocking or not, it is only read on Windows.
};

/// Structure to represent the 3ds' pollfd structure, which is different than most implementations
struct CTRPollFD {
    u32 fd; ///< Socket handle

    union Events {
        u32 hex; ///< The complete value formed by the flags
        BitField<0, 1, u32> pollin;
        BitField<1, 1, u32> pollpri;
        BitField<2, 1, u32> pollhup;
        BitField<3, 1, u32> pollerr;
        BitField<4, 1, u32> pollout;
        BitField<5, 1, u32> pollnval;

        Events& operator=(const Events& other) {
            hex = other.hex;
            return *this;
        }

        /// Translates the resulting events of a Poll operation from platform-specific to 3ds specific
        static Events TranslateTo3DS(u32 input_event) {
            Events ev = {};
            if (input_event & POLLIN)
                ev.pollin.Assign(1);
            if (input_event & POLLPRI)
                ev.pollpri.Assign(1);
            if (input_event & POLLHUP)
                ev.pollhup.Assign(1);
            if (input_event & POLLERR)
                ev.pollerr.Assign(1);
            if (input_event & POLLOUT)
                ev.pollout.Assign(1);
            if (input_event & POLLNVAL)
                ev.pollnval.Assign(1);
            return ev;
        }

        /// Translates the resulting events of a Poll operation from 3ds specific to platform specific
        static u32 TranslateToPlatform(Events input_event) {
            u32 ret = 0;
            if (input_event.pollin)
                ret |= POLLIN;
            if (input_event.pollpri)
                ret |= POLLPRI;
            if (input_event.pollhup)
                ret |= POLLHUP;
            if (input_event.pollerr)
                ret |= POLLERR;
            if (input_event.pollout)
                ret |= POLLOUT;
            if (input_event.pollnval)
                ret |= POLLNVAL;
            return ret;
        }
    };
    Events events; ///< Events to poll for (input)
    Events revents; ///< Events received (output)

    /// Converts a platform-specific pollfd to a 3ds specific structure
    static CTRPollFD FromPlatform(pollfd const& fd) {
        CTRPollFD result;
        result.events.hex = Events::TranslateTo3DS(fd.events).hex;
        result.revents.hex = Events::TranslateTo3DS(fd.revents).hex;
        result.fd = static_cast<u32>(fd.fd);
        return result;
    }

    /// Converts a 3ds specific pollfd to a platform-specific structure
    static pollfd ToPlatform(CTRPollFD const& fd) {
        pollfd result;
        result.events = Events::TranslateToPlatform(fd.events);
        result.revents = Events::TranslateToPlatform(fd.revents);
        result.fd = fd.fd;
        return result;
    }
};

/// Union to represent the 3ds' sockaddr structure
union CTRSockAddr {
    /// Structure to represent a raw sockaddr
    struct {
        u8 len; ///< The length of the entire structure, only the set fields count
        u8 sa_family; ///< The address family of the sockaddr
        u8 sa_data[0x1A]; ///< The extra data, this varies, depending on the address family
    } raw;

    /// Structure to represent the 3ds' sockaddr_in structure
    struct CTRSockAddrIn {
        u8 len; ///< The length of the entire structure
        u8 sin_family; ///< The address family of the sockaddr_in
        u16 sin_port; ///< The port associated with this sockaddr_in
        u32 sin_addr; ///< The actual address of the sockaddr_in
    } in;

    /// Convert a 3DS CTRSockAddr to a platform-specific sockaddr
    static sockaddr ToPlatform(CTRSockAddr const& ctr_addr) {
        sockaddr result;
        result.sa_family = ctr_addr.raw.sa_family;
        memset(result.sa_data, 0, sizeof(result.sa_data));

        // We can not guarantee ABI compatibility between platforms so we copy the fields manually
        switch (result.sa_family) {
        case AF_INET:
        {
            sockaddr_in* result_in = reinterpret_cast<sockaddr_in*>(&result);
            result_in->sin_port = ctr_addr.in.sin_port;
            result_in->sin_addr.s_addr = ctr_addr.in.sin_addr;
            memset(result_in->sin_zero, 0, sizeof(result_in->sin_zero));
            break;
        }
        default:
            ASSERT_MSG(false, "Unhandled address family (sa_family) in CTRSockAddr::ToPlatform");
            break;
        }
        return result;
    }

    /// Convert a platform-specific sockaddr to a 3DS CTRSockAddr
    static CTRSockAddr FromPlatform(sockaddr const& addr) {
        CTRSockAddr result;
        result.raw.sa_family = static_cast<u8>(addr.sa_family);
        // We can not guarantee ABI compatibility between platforms so we copy the fields manually
        switch (result.raw.sa_family) {
        case AF_INET:
        {
            sockaddr_in const* addr_in = reinterpret_cast<sockaddr_in const*>(&addr);
            result.raw.len = sizeof(CTRSockAddrIn);
            result.in.sin_port = addr_in->sin_port;
            result.in.sin_addr = addr_in->sin_addr.s_addr;
            break;
        }
        default:
            ASSERT_MSG(false, "Unhandled address family (sa_family) in CTRSockAddr::ToPlatform");
            break;
        }
        return result;
    }
};

// CTR's addrinfo struct
struct CTRAddrInfo {
    int                 ai_flags;
    int                 ai_family;
    int                 ai_socktype;
    int                 ai_protocol;
    socklen_t           ai_addrlen;
    u32                 ai_canonname; //char*
    u32                 ai_addr; //CTRSockAddr*
    u32                 ai_next; //CTRAddrInfo*

    static addrinfo ToPlatform(const CTRAddrInfo& ctr_info) {
        addrinfo result;
        result.ai_flags = ctr_info.ai_flags;
        result.ai_family = ctr_info.ai_family;
        result.ai_socktype = ctr_info.ai_socktype;
        result.ai_protocol = ctr_info.ai_protocol;
        result.ai_addrlen = ctr_info.ai_addrlen;
        if(ctr_info.ai_canonname != 0) {
            result.ai_canonname = ::strdup(reinterpret_cast<char*>(Memory::GetPointer(ctr_info.ai_canonname)));
        } else {
            result.ai_canonname = nullptr;
        }

        CTRSockAddr* addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(ctr_info.ai_addr));
        if(addr != nullptr) {
            result.ai_addr = new sockaddr();
            sockaddr plat_addr = CTRSockAddr::ToPlatform(*addr);
            ::memcpy(result.ai_addr, &plat_addr, sizeof(sockaddr));
        } else {
            result.ai_addr = nullptr;
        }

        return result;
    }

    static void FreePlatform(const addrinfo& info) {
        if(info.ai_canonname != nullptr) {
            delete info.ai_canonname;
        }
        if(info.ai_addr != nullptr) {
            delete info.ai_addr;
        }
    }
};

// Special addrinfo buffer format struct with specific sizes for each entry
struct CTRAddrInfoBuffer {
    s32                 ai_flags;
    s32                 ai_family;
    s32                 ai_socktype;
    s32                 ai_protocol;
    u32                 ai_addrlen;
    char                ai_canonname[256];
    sockaddr_storage    ai_addr;

    static void FromAddrInfo(const addrinfo& info, CTRAddrInfoBuffer* buffer) {
        buffer->ai_flags = info.ai_flags;
        buffer->ai_family = info.ai_family;
        buffer->ai_socktype = info.ai_socktype;
        buffer->ai_protocol = info.ai_protocol;
        buffer->ai_addrlen = info.ai_addrlen;

        size_t min_length = 0;
        if(info.ai_canonname != nullptr) {
            size_t info_name_len = ::strlen(info.ai_canonname);
            min_length = info_name_len < 256 ? info_name_len : 256;
        }

        ::strncpy(buffer->ai_canonname, info.ai_canonname, min_length);
        ::memcpy(&buffer->ai_addr, info.ai_addr, sizeof(sockaddr_storage));
    }
};

/// Holds info about the currently open sockets
static std::unordered_map<u32, SocketHolder> open_sockets;

/// Close all open sockets
static void CleanupSockets() {
    for (auto sock : open_sockets)
        closesocket(sock.second.socket_fd);
    open_sockets.clear();
}

static void Socket(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 domain = cmd_buffer[1]; // Address family
    u32 type = cmd_buffer[2];
    u32 protocol = cmd_buffer[3];

    // Only 0 is allowed according to 3dbrew, using 0 will let the OS decide which protocol to use
    if (protocol != 0) {
        cmd_buffer[1] = UnimplementedFunction(ErrorModule::SOC).raw; // TODO(Subv): Correct error code
        return;
    }

    if (domain != AF_INET) {
        cmd_buffer[1] = UnimplementedFunction(ErrorModule::SOC).raw; // TODO(Subv): Correct error code
        return;
    }

    if (type != SOCK_DGRAM && type != SOCK_STREAM) {
        cmd_buffer[1] = UnimplementedFunction(ErrorModule::SOC).raw; // TODO(Subv): Correct error code
        return;
    }

    u32 socket_handle = static_cast<u32>(::socket(domain, type, protocol));

    if ((s32)socket_handle != SOCKET_ERROR_VALUE)
        open_sockets[socket_handle] = { socket_handle, true };

    int result = 0;
    if ((s32)socket_handle == SOCKET_ERROR_VALUE)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[0] = IPC::MakeHeader(2, 2, 0);
    cmd_buffer[1] = result;
    cmd_buffer[2] = socket_handle;
}

static void Bind(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 len = cmd_buffer[2];
    CTRSockAddr* ctr_sock_addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(cmd_buffer[6]));

    if (ctr_sock_addr == nullptr) {
        cmd_buffer[1] = -1; // TODO(Subv): Correct code
        return;
    }

    sockaddr sock_addr = CTRSockAddr::ToPlatform(*ctr_sock_addr);

    int res = ::bind(socket_handle, &sock_addr, std::max<u32>(sizeof(sock_addr), len));

    int result = 0;
    if (res != 0)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[0] = IPC::MakeHeader(5, 2, 0);
    cmd_buffer[1] = result;
    cmd_buffer[2] = res;
}

static void Fcntl(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 ctr_cmd = cmd_buffer[2];
    u32 ctr_arg = cmd_buffer[3];

    int result = 0;
    u32 posix_ret = 0; // TODO: Check what hardware returns for F_SETFL (unspecified by POSIX)
    SCOPE_EXIT({
            cmd_buffer[1] = result;
            cmd_buffer[2] = posix_ret;
    });

    if (ctr_cmd == 3) { // F_GETFL
#ifdef _WIN64
        posix_ret = 0;
        auto iter = open_sockets.find(socket_handle);
        if (iter != open_sockets.end() && iter->second.blocking == false)
            posix_ret |= 4; // O_NONBLOCK
#else
        int ret = ::fcntl(socket_handle, F_GETFL, 0);
        if (ret == SOCKET_ERROR_VALUE) {
            result = TranslateError(GET_ERRNO);
            posix_ret = -1;
            return;
        }
        posix_ret = 0;
        if (ret & O_NONBLOCK)
            posix_ret |= 4; // O_NONBLOCK
#endif
    } else if (ctr_cmd == 4) { // F_SETFL
#ifdef _WIN64
        unsigned long tmp = (ctr_arg & 4 /* O_NONBLOCK */) ? 1 : 0;
        int ret = ioctlsocket(socket_handle, FIONBIO, &tmp);
        if (ret == SOCKET_ERROR_VALUE) {
            result = TranslateError(GET_ERRNO);
            posix_ret = -1;
            return;
        }
        auto iter = open_sockets.find(socket_handle);
        if (iter != open_sockets.end())
            iter->second.blocking = (tmp == 0);
#else
        int flags = ::fcntl(socket_handle, F_GETFL, 0);
        if (flags == SOCKET_ERROR_VALUE) {
            result = TranslateError(GET_ERRNO);
            posix_ret = -1;
            return;
        }

        flags &= ~O_NONBLOCK;
        if (ctr_arg & 4) // O_NONBLOCK
            flags |= O_NONBLOCK;

        int ret = ::fcntl(socket_handle, F_SETFL, flags);
        if (ret == SOCKET_ERROR_VALUE) {
            result = TranslateError(GET_ERRNO);
            posix_ret = -1;
            return;
        }
#endif
    } else {
        LOG_ERROR(Service_SOC, "Unsupported command (%d) in fcntl call", ctr_cmd);
        result = TranslateError(EINVAL); // TODO: Find the correct error
        posix_ret = -1;
        return;
    }
}

static void Listen(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 backlog = cmd_buffer[2];

    int ret = ::listen(socket_handle, backlog);
    int result = 0;
    if (ret != 0)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[0] = IPC::MakeHeader(3, 2, 0);
    cmd_buffer[1] = result;
    cmd_buffer[2] = ret;
}

static void Accept(Service::Interface* self) {
    // TODO(Subv): Calling this function on a blocking socket will block the emu thread,
    // preventing graceful shutdown when closing the emulator, this can be fixed by always
    // performing nonblocking operations and spinlock until the data is available
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    socklen_t max_addr_len = static_cast<socklen_t>(cmd_buffer[2]);
    sockaddr addr;
    socklen_t addr_len = sizeof(addr);
    u32 ret = static_cast<u32>(::accept(socket_handle, &addr, &addr_len));

    if ((s32)ret != SOCKET_ERROR_VALUE)
        open_sockets[ret] = { ret, true };

    int result = 0;
    if ((s32)ret == SOCKET_ERROR_VALUE) {
        result = TranslateError(GET_ERRNO);
    } else {
        CTRSockAddr ctr_addr = CTRSockAddr::FromPlatform(addr);
        Memory::WriteBlock(cmd_buffer[0x104 >> 2], (const u8*)&ctr_addr, max_addr_len);
    }

    cmd_buffer[0] = IPC::MakeHeader(4, 2, 2);
    cmd_buffer[1] = result;
    cmd_buffer[2] = ret;
    cmd_buffer[3] = IPC::StaticBufferDesc(static_cast<u32>(max_addr_len), 0);
}

static void GetHostId(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();

    char name[128];
    gethostname(name, sizeof(name));
    addrinfo hints = {};
    addrinfo* res;

    hints.ai_family = AF_INET;
    getaddrinfo(name, nullptr, &hints, &res);
    sockaddr_in* sock_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    in_addr* addr = &sock_addr->sin_addr;

    cmd_buffer[2] = addr->s_addr;
    cmd_buffer[1] = 0;
    freeaddrinfo(res);
}

static void Close(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];

    int ret = 0;
    open_sockets.erase(socket_handle);

    ret = closesocket(socket_handle);

    int result = 0;
    if (ret != 0)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[2] = ret;
    cmd_buffer[1] = result;
}

static void SendTo(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 len = cmd_buffer[2];
    u32 flags = cmd_buffer[3];
    u32 addr_len = cmd_buffer[4];

    u8* input_buff = Memory::GetPointer(cmd_buffer[8]);
    CTRSockAddr* ctr_dest_addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(cmd_buffer[10]));

    if (ctr_dest_addr == nullptr) {
        cmd_buffer[1] = -1; // TODO(Subv): Find the right error code
        return;
    }

    int ret = -1;
    if (addr_len > 0) {
        sockaddr dest_addr = CTRSockAddr::ToPlatform(*ctr_dest_addr);
        ret = ::sendto(socket_handle, (const char*)input_buff, len, flags, &dest_addr, sizeof(dest_addr));
    } else {
        ret = ::sendto(socket_handle, (const char*)input_buff, len, flags, nullptr, 0);
    }

    int result = 0;
    if (ret == SOCKET_ERROR_VALUE)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[2] = ret;
    cmd_buffer[1] = result;
}

static void RecvFrom(Service::Interface* self) {
    // TODO(Subv): Calling this function on a blocking socket will block the emu thread,
    // preventing graceful shutdown when closing the emulator, this can be fixed by always
    // performing nonblocking operations and spinlock until the data is available
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 len = cmd_buffer[2];
    u32 flags = cmd_buffer[3];
    socklen_t addr_len = static_cast<socklen_t>(cmd_buffer[4]);

    struct
    {
        u32 output_buffer_descriptor;
        u32 output_buffer_addr;
        u32 address_buffer_descriptor;
        u32 output_src_address_buffer;
    } buffer_parameters;

    std::memcpy(&buffer_parameters, &cmd_buffer[64], sizeof(buffer_parameters));

    u8* output_buff = Memory::GetPointer(buffer_parameters.output_buffer_addr);
    sockaddr src_addr;
    socklen_t src_addr_len = sizeof(src_addr);
    int ret = ::recvfrom(socket_handle, (char*)output_buff, len, flags, &src_addr, &src_addr_len);

    if (ret >= 0 && buffer_parameters.output_src_address_buffer != 0 && src_addr_len > 0) {
        CTRSockAddr* ctr_src_addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(buffer_parameters.output_src_address_buffer));
        *ctr_src_addr = CTRSockAddr::FromPlatform(src_addr);
    }

    int result = 0;
    int total_received = ret;
    if (ret == SOCKET_ERROR_VALUE) {
        result = TranslateError(GET_ERRNO);
        total_received = 0;
    }

    cmd_buffer[1] = result;
    cmd_buffer[2] = ret;
    cmd_buffer[3] = total_received;
}

static void Poll(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 nfds = cmd_buffer[1];
    int timeout = cmd_buffer[2];
    CTRPollFD* input_fds = reinterpret_cast<CTRPollFD*>(Memory::GetPointer(cmd_buffer[6]));
    CTRPollFD* output_fds = reinterpret_cast<CTRPollFD*>(Memory::GetPointer(cmd_buffer[0x104 >> 2]));

    // The 3ds_pollfd and the pollfd structures may be different (Windows/Linux have different sizes)
    // so we have to copy the data
    std::vector<pollfd> platform_pollfd(nfds);
    std::transform(input_fds, input_fds + nfds, platform_pollfd.begin(), CTRPollFD::ToPlatform);

    const int ret = ::poll(platform_pollfd.data(), nfds, timeout);

    // Now update the output pollfd structure
    std::transform(platform_pollfd.begin(), platform_pollfd.end(), output_fds, CTRPollFD::FromPlatform);

    int result = 0;
    if (ret == SOCKET_ERROR_VALUE)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[1] = result;
    cmd_buffer[2] = ret;
}

static void GetSockName(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    socklen_t ctr_len = cmd_buffer[2];

    CTRSockAddr* ctr_dest_addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(cmd_buffer[0x104 >> 2]));

    sockaddr dest_addr;
    socklen_t dest_addr_len = sizeof(dest_addr);
    int ret = ::getsockname(socket_handle, &dest_addr, &dest_addr_len);

    if (ctr_dest_addr != nullptr) {
        *ctr_dest_addr = CTRSockAddr::FromPlatform(dest_addr);
    } else {
        cmd_buffer[1] = -1; // TODO(Subv): Verify error
        return;
    }

    int result = 0;
    if (ret != 0)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[2] = ret;
    cmd_buffer[1] = result;
}

static void Shutdown(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    int how = cmd_buffer[2];

    int ret = ::shutdown(socket_handle, how);
    int result = 0;
    if (ret != 0)
        result = TranslateError(GET_ERRNO);
    cmd_buffer[2] = ret;
    cmd_buffer[1] = result;
}

static void GetPeerName(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    socklen_t len = cmd_buffer[2];

    CTRSockAddr* ctr_dest_addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(cmd_buffer[0x104 >> 2]));

    sockaddr dest_addr;
    socklen_t dest_addr_len = sizeof(dest_addr);
    int ret = ::getpeername(socket_handle, &dest_addr, &dest_addr_len);

    if (ctr_dest_addr != nullptr) {
        *ctr_dest_addr = CTRSockAddr::FromPlatform(dest_addr);
    } else {
        cmd_buffer[1] = -1;
        return;
    }

    int result = 0;
    if (ret != 0)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[2] = ret;
    cmd_buffer[1] = result;
}

static void Connect(Service::Interface* self) {
    // TODO(Subv): Calling this function on a blocking socket will block the emu thread,
    // preventing graceful shutdown when closing the emulator, this can be fixed by always
    // performing nonblocking operations and spinlock until the data is available
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    socklen_t len = cmd_buffer[2];

    CTRSockAddr* ctr_input_addr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(cmd_buffer[6]));
    if (ctr_input_addr == nullptr) {
        cmd_buffer[1] = -1; // TODO(Subv): Verify error
        return;
    }

    sockaddr input_addr = CTRSockAddr::ToPlatform(*ctr_input_addr);
    int ret = ::connect(socket_handle, &input_addr, sizeof(input_addr));
    int result = 0;
    if (ret != 0)
        result = TranslateError(GET_ERRNO);

    cmd_buffer[0] = IPC::MakeHeader(6, 2, 0);
    cmd_buffer[1] = result;
    cmd_buffer[2] = ret;
}

static void InitializeSockets(Service::Interface* self) {
    // TODO(Subv): Implement
#ifdef _WIN64
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif

    u32* cmd_buffer = Kernel::GetCommandBuffer();
    cmd_buffer[0] = IPC::MakeHeader(1, 1, 0);
    cmd_buffer[1] = RESULT_SUCCESS.raw;
}

static void ShutdownSockets(Service::Interface* self) {
    // TODO(Subv): Implement
    CleanupSockets();

#ifdef _WIN64
    WSACleanup();
#endif

    u32* cmd_buffer = Kernel::GetCommandBuffer();
    cmd_buffer[1] = 0;
}

static void GetSockOpt(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 level = cmd_buffer[2];
    int optname = TranslateSockOpt(cmd_buffer[3]);
    socklen_t optlen = (socklen_t)cmd_buffer[4];

    int ret = -1;
    int err = 0;

    if(optname < 0) {
#ifdef _WIN64
        err = WSAEINVAL;
#else
        err = EINVAL;
#endif
    } else {
        // 0x100 = static buffer offset (bytes)
        // + 0x4 = 2nd pointer (u32) position
        // >> 2  = convert to u32 offset instead of byte offset (cmd_buffer = u32*)
        char* optval = reinterpret_cast<char *>(Memory::GetPointer(cmd_buffer[0x104 >> 2]));

        ret = ::getsockopt(socket_handle, level, optname, optval, &optlen);
        err = 0;
        if (ret == SOCKET_ERROR_VALUE) {
            err = TranslateError(GET_ERRNO);
        }
    }

    cmd_buffer[0] = IPC::MakeHeader(0x11, 4, 2);
    cmd_buffer[1] = ret;
    cmd_buffer[2] = err;
    cmd_buffer[3] = optlen;
}

static void SetSockOpt(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 socket_handle = cmd_buffer[1];
    u32 level = cmd_buffer[2];
    int optname = TranslateSockOpt(cmd_buffer[3]);

    int ret = -1;
    int err = 0;

    if(optname < 0) {
#ifdef _WIN64
        err = WSAEINVAL;
#else
        err = EINVAL;
#endif
    } else {
        socklen_t optlen = static_cast<socklen_t>(cmd_buffer[4]);
        const char* optval = reinterpret_cast<const char*>(Memory::GetPointer(cmd_buffer[8]));

        ret = static_cast<u32>(::setsockopt(socket_handle, level, optname, optval, optlen));
        err = 0;
        if (ret == SOCKET_ERROR_VALUE) {
            err = TranslateError(GET_ERRNO);
        }
    }

    cmd_buffer[0] = IPC::MakeHeader(0x12, 4, 4);
    cmd_buffer[1] = ret;
    cmd_buffer[2] = err;
}

static void GetAddrInfo(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 buffer_len = cmd_buffer[4]; // count * sizeof(CTRAddrInfoBuffer)
    u32 max_count = buffer_len / sizeof(CTRAddrInfoBuffer);

    char* node = reinterpret_cast<char*>(Memory::GetPointer(cmd_buffer[6]));
    char* service = reinterpret_cast<char*>(Memory::GetPointer(cmd_buffer[8]));
    CTRAddrInfo* hints = reinterpret_cast<CTRAddrInfo*>(Memory::GetPointer(cmd_buffer[10]));
    CTRAddrInfoBuffer* buffer = reinterpret_cast<CTRAddrInfoBuffer*>(Memory::GetPointer(cmd_buffer[0x104 >> 2]));

    int err = 0;
    int count = 0;

    addrinfo* results;
    addrinfo* res;
    addrinfo plat_hints = CTRAddrInfo::ToPlatform(*hints);

    int ret = ::getaddrinfo(node, service, &plat_hints, &results);

    CTRAddrInfo::FreePlatform(plat_hints);

    if (ret == SOCKET_ERROR_VALUE) {
        err = TranslateError(GET_ERRNO);
    } else {
        res = results;
        while (res != nullptr && count < max_count) {
            CTRAddrInfoBuffer::FromAddrInfo(*res, buffer + (count++));
            res = res->ai_next;
        }
        freeaddrinfo(results);
    }

    cmd_buffer[0] = IPC::MakeHeader(0x0F, 4, 6);
    cmd_buffer[1] = ret;
    cmd_buffer[2] = err;
    cmd_buffer[3] = count;
}

static void GetNameInfo(Service::Interface* self) {
    u32* cmd_buffer = Kernel::GetCommandBuffer();
    u32 tmpaddr_len = cmd_buffer[1];
    u32 hostlen = cmd_buffer[2];
    u32 servlen = cmd_buffer[3];
    u32 flags = cmd_buffer[4];

    CTRSockAddr* tmpaddr = reinterpret_cast<CTRSockAddr*>(Memory::GetPointer(cmd_buffer[6]));
    char* host = reinterpret_cast<char*>(Memory::GetPointer(cmd_buffer[0x104 >> 2]));
    char* serv = reinterpret_cast<char*>(Memory::GetPointer(cmd_buffer[0x10C >> 2]));

    int err = 0;

    sockaddr addr = CTRSockAddr::ToPlatform(*tmpaddr);
    int ret = ::getnameinfo(&addr, tmpaddr_len, host, hostlen, serv, servlen, flags);

    cmd_buffer[0] = IPC::MakeHeader(0x10, 4, 2);
    cmd_buffer[1] = ret;
    cmd_buffer[2] = err;
}

const Interface::FunctionInfo FunctionTable[] = {
    {0x00010044, InitializeSockets,             "InitializeSockets"},
    {0x000200C2, Socket,                        "Socket"},
    {0x00030082, Listen,                        "Listen"},
    {0x00040082, Accept,                        "Accept"},
    {0x00050084, Bind,                          "Bind"},
    {0x00060084, Connect,                       "Connect"},
    {0x00070104, nullptr,                       "recvfrom_other"},
    {0x00080102, RecvFrom,                      "RecvFrom"},
    {0x00090106, nullptr,                       "sendto_other"},
    {0x000A0106, SendTo,                        "SendTo"},
    {0x000B0042, Close,                         "Close"},
    {0x000C0082, Shutdown,                      "Shutdown"},
    {0x000D0082, nullptr,                       "GetHostByName"},
    {0x000E00C2, nullptr,                       "GetHostByAddr"},
    {0x000F0106, GetAddrInfo,                   "GetAddrInfo"},
    {0x00100102, GetNameInfo,                   "GetNameInfo"},
    {0x00110102, GetSockOpt,                    "GetSockOpt"},
    {0x00120104, SetSockOpt,                    "SetSockOpt"},
    {0x001300C2, Fcntl,                         "Fcntl"},
    {0x00140084, Poll,                          "Poll"},
    {0x00150042, nullptr,                       "SockAtMark"},
    {0x00160000, GetHostId,                     "GetHostId"},
    {0x00170082, GetSockName,                   "GetSockName"},
    {0x00180082, GetPeerName,                   "GetPeerName"},
    {0x00190000, ShutdownSockets,               "ShutdownSockets"},
    {0x001A00C0, nullptr,                       "GetNetworkOpt"},
    {0x001B0040, nullptr,                       "ICMPSocket"},
    {0x001C0104, nullptr,                       "ICMPPing"},
    {0x001D0040, nullptr,                       "ICMPCancel"},
    {0x001E0040, nullptr,                       "ICMPClose"},
    {0x001F0040, nullptr,                       "GetResolverInfo"},
    {0x00210002, nullptr,                       "CloseSockets"},
    {0x00230040, nullptr,                       "AddGlobalSocket"},
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// Interface class

Interface::Interface() {
    Register(FunctionTable);
}

Interface::~Interface() {
    CleanupSockets();
#ifdef _WIN64
    WSACleanup();
#endif
}

} // namespace
