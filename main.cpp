#include <QCoreApplication>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <iostream>
#include <QDebug>
#include <chrono>
#include <QDateTime>

#define NTP_PORT 123
#define NTP_TIMESTAMP_DELTA 2208988800ULL

struct ntp_packet {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t rootDelay;
    uint32_t rootDispersion;
    uint32_t refId;
    uint32_t refTm_s;
    uint32_t refTm_f;
    uint32_t origTm_s;
    uint32_t origTm_f;
    uint32_t rxTm_s;
    uint32_t rxTm_f;
    uint32_t txTm_s;
    uint32_t txTm_f;
};

QString ntp_to_qstring(uint32_t sec, uint32_t frac)
{
    uint64_t s = ntohl(sec);
    uint64_t f = ntohl(frac);

    // Convert NTP → Unix time
    double frac_sec = (double)f / (double)(1ULL << 32);
    double unix_time = (double)(s - NTP_TIMESTAMP_DELTA) + frac_sec;

    // Split seconds + nanoseconds
    qint64 secs = static_cast<qint64>(unix_time);
    qint64 nsec = static_cast<qint64>((unix_time - secs) * 1e9);

    QDateTime dt = QDateTime::fromSecsSinceEpoch(secs, Qt::UTC);

    return QString("%1.%2")
            .arg(dt.toString("yyyy-MM-dd HH:mm:ss"))
            .arg(nsec, 9, 10, QChar('0'));
}


uint64_t ntp_time_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto sec = duration_cast<seconds>(now.time_since_epoch()).count();
    auto frac = duration_cast<nanoseconds>(now.time_since_epoch()).count() % 1000000000;
    uint64_t ntp_sec = sec + NTP_TIMESTAMP_DELTA;
    uint64_t ntp_frac = (uint64_t)((double)frac * (double)(1ULL << 32) / 1e9);
    return (ntp_sec << 32) | ntp_frac;
}

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(NTP_PORT);

    if (bind(sockfd, (sockaddr*)&server, sizeof(server)) < 0)
    {
        perror("bind");
        return 1;
    }

    qDebug() << "RFC-5905 compatible NTP server running\n";

    while (true)
    {
        ntp_packet req{}, resp{};
        sockaddr_in client{};
        socklen_t len = sizeof(client);

        if (recvfrom(sockfd, &req, sizeof(req), 0,
                     (sockaddr*)&client, &len) < 0)
            continue;

        uint64_t rx_time = ntp_time_now();

        // LI = 0, VN = 4, Mode = 4 (server)
        resp.li_vn_mode = (0 << 6) | (4 << 3) | 4;
        //resp.stratum = 1;                 // Primary server
        resp.stratum = 2;                 // Secondary server
        resp.poll = req.poll;
        resp.precision = -20;

        resp.rootDelay = htonl(1 << 16);
        resp.rootDispersion = htonl(1 << 16);

        // Reference ID: "LOCL"
        //resp.refId = htonl(0x4C4F434C);
        resp.refId   = htonl(inet_addr("127.0.0.1"));

        // Reference timestamp = current time
        uint64_t ref_time = rx_time;
        resp.refTm_s = htonl(ref_time >> 32);
        resp.refTm_f = htonl(ref_time & 0xFFFFFFFF);

        // Originate timestamp = client's transmit timestamp
        resp.origTm_s = req.txTm_s;
        resp.origTm_f = req.txTm_f;

        // Receive timestamp
        resp.rxTm_s = htonl(rx_time >> 32);
        resp.rxTm_f = htonl(rx_time & 0xFFFFFFFF);

        // Transmit timestamp
        uint64_t tx_time = ntp_time_now();
        resp.txTm_s = htonl(tx_time >> 32);
        resp.txTm_f = htonl(tx_time & 0xFFFFFFFF);



        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, client_ip, sizeof(client_ip));

        uint8_t version = (req.li_vn_mode >> 3) & 0x7;
        uint8_t mode    = req.li_vn_mode & 0x7;

        qDebug() << "[NTP] Client=" << client_ip
                  << ":" << ntohs(client.sin_port)
                  << "  VN=" << (int)version
                  << "  Mode=" << (int)mode;

        qDebug() << "[NTP] ORIG =" << ntp_to_qstring(req.txTm_s, req.txTm_f);
        qDebug() << "[NTP] RX   =" << ntp_to_qstring(resp.rxTm_s, resp.rxTm_f);
        qDebug() << "[NTP] TX   =" << ntp_to_qstring(resp.txTm_s, resp.txTm_f);

        sendto(sockfd, &resp, sizeof(resp), 0,
               (sockaddr*)&client, len);
    }

    close(sockfd);

    return a.exec();

}
