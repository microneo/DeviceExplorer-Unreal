# Security

Report vulnerabilities privately to the repository maintainers. Do not include
tokens, device logs, personal data, or unpublished project content in a public
issue.

## Threat model

DeviceExplorer is a development tool for a local network. It assumes the host and
the device are operated by the same developer, and that a connected host is
trusted: **an authenticated host has unrestricted console access to the build**,
which is the purpose of the tool, not a defect. Do not connect a build to a host
you do not control.

Everything else is treated as hostile, including other hosts on the same network.

## Properties to keep intact

- The dashboard listener binds to loopback only, and rejects requests whose `Host`
  or `Origin` is not loopback, and non-GET requests without the dashboard's own
  request header. That keeps a page in the developer's browser from driving the
  API.
- Discovery advertises only a fingerprint of the session token. The token never
  crosses the network, in a TXT record, a URL, or a header.
- A client obtains the token out of band: from the project settings, from
  `-DeviceExplorerToken`, or from the Editor for a client in its own process. A client
  with no token does not connect.
- Host and device each prove knowledge of the token over the open socket before
  either sends anything else, with both nonces bound into every proof. A host that
  copied a fingerprint from the network cannot complete the handshake.
- File access is limited to registered roots, resolved and prefix-checked against
  the root, and downloads require the root to opt in.
- Upload URLs carry a single-use per-transfer credential, not the session token.
- The runtime client is excluded from Shipping builds.

## Known limits

- A project session token is committed with the project and packaged into its builds.
  It is therefore extractable from any build you distribute, and holding it is enough
  to connect to a host running it. Treat it as a secret shared with everyone who has
  the project or one of its builds — rotate it by clearing the setting, and prefer a
  token per host session when that group is wider than the group you would give
  console access to.
- Traffic is not encrypted. Anyone on the segment can read logs, file contents, and
  command output in transit, and the fingerprint identifies which host is running.
- The handshake authenticates both ends but derives no session key. An attacker who
  can relay traffic between a real client and a real host — a rogue access point, ARP
  spoofing — passes both proofs untouched and can then read and inject messages on the
  established connection. Authentication stops impersonation, not relaying.
- An unauthenticated peer can hold a device connection open without completing the
  handshake; there is no idle deadline yet.
- The device listener binds all interfaces so devices on the LAN can reach it.
