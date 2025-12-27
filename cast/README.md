# libcast

libcast is an open source implementation of the Cast protocols that allow Cast
senders to launch Cast applications and stream real-time media to
Cast-compatible devices (aka "receivers").

Included are two applications, `cast_sender` and `cast_receiver` that
demonstrate how to send and receive media using a Cast Streaming session.

## Components

Libcast's components include the following:

* [cast/streaming/](streaming/README.md) - Cast Streaming (both sending and receiving media).
* [receiver/public/](receiver/public/README.md) - Cast server socket and a demonstration
    server (agent).
* [sender/public/](sender/public/README.md) - Cast client socket and supporting APIs to
    launch Cast applications.
* [test/](test/README.md) - Integration tests.

With all of the documentation for this implementation in the [docs](docs/)
folder.

> The `streaming` module can be used independently of the `sender` and `receiver`
> modules.
