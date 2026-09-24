# Reaching the device over the network

Shared by the evb-use, evb-flash and evb-release skills.

The firmware gets its address from DHCP (Ethernet first, or WiFi STA per
`wifi.network_policy`) and advertises itself over mDNS as
`<hostname>.local` (default hostname `esp32-evb-relay`). The REST API is
plain HTTP on port 80 under `/api/v1`.

## Find the address, in this order

1. The user or environment already names it: `EVB_RELAY_HOST`, `-H`, or
   `host` in the CLI config file. Use that.
2. `evb-relay discover` browses mDNS and lists devices with their IPs.
   <!-- sync: evb-qv50.17/.19/.20 -->
   Discovery options (interface selection, all-interface browsing) are
   being reworked; check `evb-relay discover --help` for the current flags.
3. mDNS finds nothing: it is often blocked, not broken. Sandboxed or
   containerized agent shells, VPNs and separate VLANs drop multicast.
   Ask the user for the IP, or read it from the DHCP server or router,
   and pass it directly with `-H <ip>`. A direct IP always works when
   unicast HTTP gets through.
4. Serial access and nothing else: after reset the boot log prints
   `Ethernet got IP: ip=<addr>`. Reading the console resets nothing but
   needs exclusive use of the port (see `skills/_shared/serial-port.md`).

## Check it is the right device

`evb-relay -H <ip> status` without a valid token fails with
`AUTH_REQUIRED` or `AUTH_FORBIDDEN` (exit 3). Either error proves an EVB
relay is listening there; `NETWORK_ERROR` (exit 2) means nothing
answered. With a token, `status` shows firmware version, uptime and
network details. A board with no token provisioned rejects every API
request until one is written (see the evb-flash skill).
