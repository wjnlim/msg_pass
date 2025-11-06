# Message Passing Library

A simple, line-based **asynchronous message passing library** built on top of my [ep_engine](https://github.com/wjnlim/ep_engine.git) (an epoll-based event engine).

---

## Overview

This library provides a way to implement lightweight **asynchronous server–client model** using line-based messages.  
<!-- Each message is assumed to end with a **newline character (`\n`)**. -->

It offers:
- A non-blocking, epoll-based I/O model  
- Asynchronous message handling  

---

## Features

- **Line-based message protocol**  
  Messages are considered complete when a newline (`\n`) is received.

- **Asynchronous server–client model**  
  Uses an event-driven architecture built on [ep_engine](https://github.com/wjnlim/ep_engine.git).

- **Callback-driven message handling**  
  - `mp_server` accepts a user-provided **request handler callback**.  
    - The callback is triggered whenever the server receives a message from a client.  
    - The handler can send a reply message back to that client.
  - `mp_client` accepts a user-provided **on_reply callback**.  
    - The client can send requests to the server.  
    - The `on_reply` callback is called asynchronously when the client receives a reply.
- **Notes**
  - This project is mainly for personal use, not for production codes.\
  Thus, the code may lack thorough testing, so please use it with caution.
  - This project uses my [ep_engine](https://github.com/wjnlim/ep_engine.git) library. The CMake file will automatically fetch the project internally
---

## Build and Installation

Follow these steps to build and install the library:

```bash
# 1. Clone the repository
git clone https://github.com/wjnlim/msg_pass.git

# 2. Create a build directory
mkdir msg_pass/build
cd msg_pass/build

# 3. Configure with CMake
cmake -DCMAKE_INSTALL_PREFIX=<your install directory> ..

# 4. Build and install the library
cmake --build . --target install
```

## Usage Example
Refer to the demo program in the repository for example usage.
* [hello_server.c](tests/hello_server.c)
* [hello_client.c](tests/hello_client.c)

To compile them:
```bash
# hello_server
gcc hello_server.c -o hello_server -I <your install directory>/include/ <your install directory>/lib/libmsg_pass.a -lpthread

# hello_client
gcc hello_client.c -o hello_client -I <your install directory>/include/ <your install directory>/lib/libmsg_pass.a -lpthread
```
To run them:
```bash
# demo server
./hello_server

# demon client
./hello_client <server ip> <client name> <use_threadpool> <number_of_requests>
```

To compile your program using this library (note that the **pthread** library must be linked with also):
```bash
gcc your_prog.c -o your_prog -I <your install directory>/include \
  <your install directory>/lib/libep_engine.a -lpthread
```