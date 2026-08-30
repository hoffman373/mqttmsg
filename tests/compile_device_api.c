/* Compiles the client's public header with a host compiler that has no Pico
   SDK on its include path.

   Nothing here runs. The whole assertion is that this file builds: the
   header an application includes to publish a message must not name an lwIP
   or Pico SDK type, so that the same application source can be built against
   a different transport. That property is easy to lose by accident — one
   convenience #include of transport_pico.h takes it away, and every Pico
   build keeps working, so nothing else would notice.

   Choosing a transport is deliberately still platform-specific: that is what
   transport_pico.h and transport_socket.h are, and both are meant to be named
   explicitly by whoever picks one. */

#include <mqttmsg/mqttmsg.h>
