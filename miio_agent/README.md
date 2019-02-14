# miio_agent

# How to compile
```
# autoreconf -i
# ./configure
# make
```

# How to use
1. connect and register
    * use UNIX socket, path is "/tmp/miio_agent.socket"
    * for ot message, register methods first
    ```
        "{"method":"register","key":"%s"}"
    ```
    * for local message, bind address first
    ```
        "{"method":"bind","address":%u}"

        #define MIIO_AGENT_CLIENT_ADDRESS(__num) ((1<<(__num)) & 0xFFFFFFFF)
    ```
    * the max number of clients to be supported is 32, all unicast, multicast, broadcast supported.

2. send/recv
    - for local json rpc, send insert `"_to:%u`, recv insert `"_from:%u`
