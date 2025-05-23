# Open Screen Protocol

Information about the Open Screen Protocol and its specification can be found
[on GitHub](https://w3c.github.io/openscreenprotocol/).

*IMPORTANT:* This implementation is not yet up-to-date with the current draft
specification or feature-complete.  Public APIs can and will change without any
advance notice.  _Use at your own risk._

## API conventions

The public API for the protocol implementation is contained entirely in
`public/`.  Many functions and objects in the public API take raw pointers to
delegate, observer, callback, or client objects to get notified of relevant
state changes.

Unless specifically documented otherwise, these pointer arguments *must* never
be passed as `nullptr` and the pointee *must* persist beyond the lifetime of the
object that they are passed into.

In the future, we are considering alternatives to this calling convention to
make it simpler for clients to interact with the public API.

## Entire Flow Chart

The following entire flow chart is a copy from [Open Screen Spec](https://w3c.github.io/openscreenprotocol/).
It shows the entire process when agents have not authenticated each other,
including discovery, QUIC connection establishment, metadata exchange and
authentication.

*IMPORTANT:* This implementation is not yet up-to-date with the current draft
specification or feature-complete. So some steps in the chart are missing in
current implementation.

![Entire Flow Chart](./docs/entire_flow_chart.svg)
