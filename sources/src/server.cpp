// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
#include "server.h"
#include "paths.h"

#ifdef _WIN32
  #ifndef NOMINMAX
   #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  #define CLOSESOCK closesocket
  #define POLL WSAPoll
  static bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <cerrno>
  using socket_t = int;
  #define CLOSESOCK close
  #define POLL poll
  static bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

namespace jsg
{

namespace
{
    //--------------------------------------------------------------------------
    // SHA-1 et base64 : pour la poignée de main WebSocket
    std::string sha1 (const std::string& input)
    {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        std::string msg = input;
        const uint64_t bits = (uint64_t) input.size() * 8;
        msg += (char) 0x80;
        while (msg.size() % 64 != 56) msg += (char) 0;
        for (int i = 7; i >= 0; --i) msg += (char) ((bits >> (i * 8)) & 0xff);

        const auto rol = [] (uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

        for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = ((uint32_t) (uint8_t) msg[chunk + i * 4] << 24) | ((uint32_t) (uint8_t) msg[chunk + i * 4 + 1] << 16)
                     | ((uint32_t) (uint8_t) msg[chunk + i * 4 + 2] << 8) | (uint32_t) (uint8_t) msg[chunk + i * 4 + 3];
            for (int i = 16; i < 80; ++i) w[i] = rol (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
                const uint32_t temp = rol (a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol (b, 30); b = a; a = temp;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }

        std::string digest;
        for (uint32_t v : h) for (int i = 3; i >= 0; --i) digest += (char) ((v >> (i * 8)) & 0xff);
        return digest;
    }

    std::string base64 (const std::string& in)
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3)
        {
            const uint32_t v = ((uint8_t) in[i] << 16) | ((uint8_t) in[i + 1] << 8) | (uint8_t) in[i + 2];
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += table[v & 63];
        }
        if (i + 1 == in.size())
        {
            const uint32_t v = (uint8_t) in[i] << 16;
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += "==";
        }
        else if (i + 2 == in.size())
        {
            const uint32_t v = ((uint8_t) in[i] << 16) | ((uint8_t) in[i + 1] << 8);
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    std::string mimeFor (const std::filesystem::path& p)
    {
        static const std::map<std::string, std::string> types {
            { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" },
            { ".css", "text/css; charset=utf-8" },   { ".wasm", "application/wasm" },
            { ".webp", "image/webp" },               { ".png", "image/png" },
            { ".ico", "image/x-icon" },              { ".mp3", "audio/mpeg" },
            { ".wav", "audio/wav" },                 { ".json", "application/json" },
            { ".txt", "text/plain; charset=utf-8" },
        };
        auto ext = p.extension().string();
        std::transform (ext.begin(), ext.end(), ext.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        auto it = types.find (ext);
        return it != types.end() ? it->second : "application/octet-stream";
    }

    std::string urlDecode (const std::string& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '%' && i + 2 < s.size()) { out += (char) std::stoi (s.substr (i + 1, 2), nullptr, 16); i += 2; }
            else out += s[i];
        }
        return out;
    }

    void setNonBlocking (socket_t s)
    {
       #ifdef _WIN32
        u_long on = 1; ioctlsocket (s, FIONBIO, &on);
       #else
        fcntl (s, F_SETFL, fcntl (s, F_GETFL, 0) | O_NONBLOCK);
       #endif
        int one = 1;
        setsockopt (s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof one);
    }
}

//==============================================================================
struct Server::Connection
{
    int id = 0;
    socket_t sock;
    bool websocket = false, closing = false, peerGone = false;
    std::string in, out;                  // octets reçus non traités, octets à envoyer
    std::vector<uint8_t> message;         // message WebSocket en cours (fragments)
    int messageOpcode = 0;
};

Server::Server()
{
   #ifdef _WIN32
    WSADATA data;
    WSAStartup (MAKEWORD (2, 2), &data);
   #endif
}

Server::~Server()
{
    for (auto* c : connections) { CLOSESOCK (c->sock); delete c; }
    if (listener != -1) CLOSESOCK ((socket_t) listener);
   #ifdef _WIN32
    WSACleanup();
   #endif
}

int Server::listen (const std::vector<int>& ports)
{
    for (int port : ports)
    {
        socket_t s = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
       #ifdef _WIN32
        if (s == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        setsockopt (s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &exclusive, sizeof exclusive);
       #else
        if (s < 0) continue;
        int reuse = 1;
        setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
       #endif
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons ((uint16_t) port);
        addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

        if (bind (s, (sockaddr*) &addr, sizeof addr) == 0 && ::listen (s, 16) == 0)
        {
            setNonBlocking (s);
            listener = (intptr_t) s;
            return port;
        }
        CLOSESOCK (s);
    }
    return 0;
}

int Server::numClients() const
{
    int n = 0;
    for (auto* c : connections) if (c->websocket && ! c->closing) ++n;
    return n;
}

void Server::run()
{
    while (running)
    {
        std::vector<pollfd> fds;
        fds.push_back ({ (socket_t) listener, POLLIN, 0 });
        for (auto* c : connections)
            fds.push_back ({ c->sock, (short) (POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0 });

        POLL (fds.data(), (unsigned) fds.size(), 10);

        if (fds[0].revents & POLLIN)
        {
            while (true)
            {
                socket_t s = accept ((socket_t) listener, nullptr, nullptr);
               #ifdef _WIN32
                if (s == INVALID_SOCKET) break;
               #else
                if (s < 0) break;
               #endif
                setNonBlocking (s);
                auto* c = new Connection();
                c->id = nextId++;
                c->sock = s;
                connections.push_back (c);
            }
        }

        for (size_t k = 0; k < connections.size(); ++k)
        {
            auto& c = *connections[k];
            const auto revents = k + 1 < fds.size() ? fds[k + 1].revents : 0;

            if (revents & (POLLIN | POLLHUP | POLLERR))
            {
                char buffer[65536];
                while (true)
                {
                    const auto got = recv (c.sock, buffer, sizeof buffer, 0);
                    if (got > 0) { c.in.append (buffer, (size_t) got); continue; }
                    if (got == 0 || ! wouldBlock()) c.closing = c.peerGone = true;
                    break;
                }
                if (c.websocket) handleFrames (c); else handleHttp (c);
            }

            if (! c.out.empty())
            {
                const auto sent = send (c.sock, c.out.data(), (int) std::min<size_t> (c.out.size(), 1 << 20), 0);
                if (sent > 0) c.out.erase (0, (size_t) sent);
                else if (sent < 0 && ! wouldBlock()) c.closing = c.peerGone = true;
            }
        }

        // Connexions terminées (HTTP : une fois la réponse partie)
        for (size_t k = 0; k < connections.size();)
        {
            auto* c = connections[k];
            if (c->closing && (c->out.empty() || c->peerGone))
            {
                if (c->websocket && onClose) onClose (c->id);
                CLOSESOCK (c->sock);
                delete c;
                connections.erase (connections.begin() + (long) k);
            }
            else ++k;
        }

        if (onTick) onTick();
    }
}

//==============================================================================
void Server::handleHttp (Connection& c)
{
    const auto end = c.in.find ("\r\n\r\n");
    if (end == std::string::npos) return;
    const std::string request = c.in.substr (0, end);
    c.in.erase (0, end + 4);

    std::string method, target;
    {
        const auto sp1 = request.find (' '), sp2 = request.find (' ', sp1 + 1);
        method = request.substr (0, sp1);
        target = request.substr (sp1 + 1, sp2 - sp1 - 1);
    }

    std::map<std::string, std::string> headers;
    size_t pos = request.find ("\r\n");
    while (pos != std::string::npos && pos + 2 < request.size())
    {
        const auto next = request.find ("\r\n", pos + 2);
        const auto line = request.substr (pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        const auto colon = line.find (':');
        if (colon != std::string::npos)
        {
            auto key = line.substr (0, colon);
            std::transform (key.begin(), key.end(), key.begin(), [] (unsigned char ch) { return (char) std::tolower (ch); });
            auto value = line.substr (colon + 1);
            while (! value.empty() && value.front() == ' ') value.erase (0, 1);
            headers[key] = value;
        }
        pos = next;
    }

    auto path = target.substr (0, target.find ('?'));

    if (path == "/ws" && headers.count ("sec-websocket-key"))
    {
        const auto accept = base64 (sha1 (headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        c.websocket = true;
        if (onOpen) onOpen (c.id);
        handleFrames (c);
        return;
    }

    std::string status = "200 OK", type = "text/plain; charset=utf-8", body;

    if (method != "GET" && method != "HEAD")
    {
        status = "405 Method Not Allowed";
    }
    else
    {
        path = urlDecode (path);
        if (path == "/") path = "/index.html";
        std::error_code ec;
        std::filesystem::path file;
        bool found = false;

        if (path.find ("..") == std::string::npos)
        {
            // Dossiers supplémentaires d'abord (ex. /playbacks/ -> Musique/JSG Guitar/Playbacks), puis l'interface
            for (const auto& [prefix, folder] : extraFolders)
            {
                if (path.rfind (prefix, 0) != 0) continue;
                const auto candidate = (folder / fromUtf8 (path.substr (prefix.size()))).lexically_normal();
                if (toUtf8 (candidate).rfind (toUtf8 (folder.lexically_normal()), 0) == 0 && std::filesystem::is_regular_file (candidate, ec))
                {
                    file = candidate;
                    found = true;
                    break;
                }
            }
            if (! found)
            {
                const auto candidate = (root / fromUtf8 (path.substr (1))).lexically_normal();
                found = toUtf8 (candidate).rfind (toUtf8 (root.lexically_normal()), 0) == 0 && std::filesystem::is_regular_file (candidate, ec);
                if (found) file = candidate;
            }
        }

        if (found)
        {
            std::ifstream f (file, std::ios::binary);
            body.assign (std::istreambuf_iterator<char> (f), {});
            type = mimeFor (file);
        }
        else
        {
            status = "404 Not Found";
            body = "Introuvable";
        }
    }

    c.out += "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string (body.size())
           + "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    if (method != "HEAD") c.out += body;
    c.closing = true;
}

void Server::handleFrames (Connection& c)
{
    while (true)
    {
        if (c.in.size() < 2) return;
        const auto* b = (const uint8_t*) c.in.data();
        const bool fin = (b[0] & 0x80) != 0;
        const int opcode = b[0] & 0x0f;
        const bool masked = (b[1] & 0x80) != 0;
        uint64_t length = b[1] & 0x7f;
        size_t header = 2;

        if (length == 126) { if (c.in.size() < 4) return; length = ((uint64_t) b[2] << 8) | b[3]; header = 4; }
        else if (length == 127)
        {
            if (c.in.size() < 10) return;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | b[2 + i];
            header = 10;
        }

        if (length > (uint64_t) 200 * 1024 * 1024) { c.closing = true; return; }   // 200 Mo au plus
        const size_t maskAt = header;
        if (masked) header += 4;
        if (c.in.size() < header + length) return;

        std::vector<uint8_t> payload ((size_t) length);
        for (size_t i = 0; i < (size_t) length; ++i)
            payload[i] = (uint8_t) c.in[header + i] ^ (masked ? (uint8_t) c.in[maskAt + (i & 3)] : 0);
        c.in.erase (0, header + (size_t) length);

        if (opcode == 8) { sendFrame (c, 8, nullptr, 0); c.closing = true; return; }
        if (opcode == 9) { sendFrame (c, 10, payload.data(), payload.size()); continue; }
        if (opcode == 10) continue;

        if (opcode == 1 || opcode == 2) { c.message = std::move (payload); c.messageOpcode = opcode; }
        else if (opcode == 0)           c.message.insert (c.message.end(), payload.begin(), payload.end());

        if (! fin) continue;

        if (c.messageOpcode == 1 && onText)   onText (c.id, std::string (c.message.begin(), c.message.end()));
        if (c.messageOpcode == 2 && onBinary) onBinary (c.id, c.message);
        c.message.clear();
    }
}

void Server::sendFrame (Connection& c, int opcode, const void* data, size_t size)
{
    std::string frame;
    frame += (char) (0x80 | opcode);
    if (size < 126) frame += (char) size;
    else if (size < 65536) { frame += (char) 126; frame += (char) (size >> 8); frame += (char) (size & 0xff); }
    else { frame += (char) 127; for (int i = 7; i >= 0; --i) frame += (char) ((uint64_t) size >> (i * 8)); }
    c.out += frame;
    if (size > 0) c.out.append ((const char*) data, size);
}

void Server::sendText (int client, const std::string& text)
{
    for (auto* c : connections) if (c->id == client && c->websocket && ! c->closing) sendFrame (*c, 1, text.data(), text.size());
}

void Server::sendBinary (int client, const void* data, size_t size)
{
    for (auto* c : connections) if (c->id == client && c->websocket && ! c->closing) sendFrame (*c, 2, data, size);
}

void Server::broadcastText (const std::string& text)
{
    for (auto* c : connections) if (c->websocket && ! c->closing) sendFrame (*c, 1, text.data(), text.size());
}

void Server::broadcastBinary (const void* data, size_t size)
{
    for (auto* c : connections) if (c->websocket && ! c->closing) sendFrame (*c, 2, data, size);
}

} // namespace jsg
