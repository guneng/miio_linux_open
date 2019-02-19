#ifndef __MIIO_AGENT_H
#define __MIIO_AGENT_H

#define MIIO_AGENT_SERVER_PATH "/tmp/miio_agent.socket"

#define MIIO_AGENT_CLIENT_MAX_NUM 32
#define MIIO_AGENT_CLIENT_ADDRESS(__num) ((1<<((__num)-1)) & 0xFFFFFFFF)

#define MIIO_AGENT_MAX_KEY_NUM    100
#define MIIO_AGENT_MAX_KEY_LEN    32
#define MIIO_AGENT_MAX_MSG_LEN    1024
#define MIIO_AGENT_MAX_VALID_TIME 5 /* 5s */
#define MIIO_AGENT_MAX_ID_NUM     2147483647

#ifndef CC_INLINE
#define CC_INLINE inline
#endif

#endif
