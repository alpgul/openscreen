# libcast High-Level Architecture

## Introduction

The `libcast` library implements the core Chromecast protocols, enabling
discovery, application control, and real-time media streaming between sender and
receiver devices. The library is designed to be modular and portable, relying on
a platform abstraction layer for easy integration into various applications and
operating systems.

## Core Components

The library is primarily composed of four major components: the Sender, the
Receiver, the Streaming component, and the Platform API.

### Sender

The Sender is the client component that initiates communication. Its primary
roles are:

- **Discovery:** Finding Cast-enabled receivers on the local network using
    mDNS.
- **Session Management:** Establishing a secure connection to a receiver,
    launching applications, and managing the application lifecycle.
- **Media Control:** Initiating, pausing, and stopping media playback. It
    communicates with the Streaming Component to handle the media pipeline.

### Receiver

The Receiver is the server component running on a Cast device. Its primary roles
are:

- **Discoverability:** Announcing its presence on the network so senders can
    find it.
- **Connection Handling:** Accepting connections from senders and managing the
    secure CastV2 channel.
- **Application Management:** Launching and terminating applications (e.g., a
    media player app) based on requests from the sender.
- **Media Playback:** Receiving and rendering media streams delivered by the
    Streaming Component.

### Streaming component

The Streaming component manages the real-time transport of media. It is used by
both the Sender (for encoding and sending) and the Receiver (for receiving and
decoding).

- **Session Negotiation:** Implements the offer/answer protocol (similar to
    WebRTC) to negotiate media formats, codecs, and network parameters.
- **Media Transport:** Manages the RTP/RTCP sessions for efficient and
    synchronized delivery of audio and video frames.
- **Encoding/Decoding Hooks:** Provides the framework for plugging in hardware
    or software codecs, but does not implement the codecs themselves.

### Platform API

The Platform API is a crucial abstraction layer that decouples the core
`libcast` logic from any specific operating system or hardware. It defines a set
of interfaces that the embedding application must implement.

- **Responsibilities:** Provides platform-specific implementations for
    networking (sockets, mDNS), threading (clocks, tasks), and logging.
- **Portability:** By requiring the host application to provide these
    implementations, `libcast` remains highly portable across different
    environments, from embedded devices to desktop applications.

## Interaction Diagram

The following diagram illustrates how these components interact. An external
application (e.g., a mobile app) uses the libcast Sender API. The `libcast`
sender then leverages the Platform API to communicate over the network with a
`libcast` Receiver, which in turn drives a receiver-side application.

Note that both the sender and receiver must have complete platform
implementations, and both depend on the streaming component.

```ascii
+---------------------+      +--------------------------+<---------------------+
| Sender Application  |----->| Discovery (usually mDNS) |                      |
+---------------------+      +--------------------------+------+               |
          |                                                    |               |
          v                                                    V               |
+---------------------+                                +-------------------+   |
|   libcast Sender    |                                |                   |   |
+---------------------+                                |   Sender          |   |
          |                                            |   Platform API    |   |
          |              +------------------+          |  (Network, etc.)  |   |
          |-----+------->| CastV2 Protocol  |--------->|                   |   |
          |     |        +------------------+          +-------+----+------+   |
          |     |                                              |    ^          |
          |     |        +------------------+                  |    |          |
          +-----x------->|    Streaming     |------------------+    |          |
                |        +------------------+                       |          |
                |               |                               (Network)      |
                |               |                                   |          |
                |               |                                   V          |
                |      +---------------------+         +-------------------+   |
                +------+   libcast Receiver  |-------->|   Receiver        |   |
                       +---------------------+         |   Platform API    |   |
                                 |                     |   (Network, etc.) |   |
                                 v                     +-------------------+   |
                       +----------------------+                                |
                       | Receiver Application +--------------------------------+
                       +----------------------+
```
