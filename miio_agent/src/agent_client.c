/*
 * Simple agent client for your reference.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "miio_agent.h"

#define DATA_MAX	1024

/*
 * "{"method":"register","key":"%s"}"
 * "{"method":"unregister","key":"%s"}"
 * "{"method":"unregister","key":"%s"}"  [> if key is null, unregister all <]
 */

char *reg_template1 = "{\"method\":\"register\",\"key\":\"set_light\"}";
char *reg_template2 = "{\"method\":\"register\",\"key\":\"set_watermark\"}";
#if 0
char *unreg_template1 = "{\"method\":\"unregister\",\"key\":\"keya\"}";
char *unreg_template2 = "{\"method\":\"unregister\",\"key\":\"keyc\"}";
char *unreg_template3 = "{\"method\":\"unregister\",\"key\":\"\"}";
#endif

int main(int argc, char**argv)
{
    int sockfd, n;
    struct sockaddr_un servaddr;
    char buf[DATA_MAX];

    if (argc != 2 || !(atoi(argv[1]) > 0)) {
        printf("Usage: %s number\n", argv[0]);
        return 1;
    }

    sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sun_family = AF_UNIX;
    strcpy(servaddr.sun_path, MIIO_AGENT_SERVER_PATH);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        printf("Connect to server error: %s\n", MIIO_AGENT_SERVER_PATH);
        return -1;
    }

    n = send(sockfd, reg_template1, strlen(reg_template1), 0);
    /* printf("sub key:%s send ret : %d\n", reg_template1, n); */

    n = send(sockfd, reg_template2, strlen(reg_template2), 0);
    /* printf("sub key:%s send ret : %d\n", reg_template2, n); */

    uint32_t address = MIIO_AGENT_CLIENT_ADDRESS(atoi(argv[1]));
    n = sprintf(buf, "{\"method\":\"bind\",\"address\":%u}", address);
    n = send(sockfd, buf, n, 0);
    /* printf("reg address:%s send ret : %d\n", buf, n); */

    while (1) {
        n = recv(sockfd, buf, sizeof(buf), 0);
        if (n > 0) {
            buf[n] = '\0';
            printf("client %8.8X recv msg is %s\n", address, buf);
        } else {
            break;
        }
    }

    return 0;
}
