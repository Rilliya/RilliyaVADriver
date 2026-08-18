# Changelog

This project is pre-alpha. The public API, catalog schema, and product-configuration contract may
change before 1.0.0. Each tagged release records user-visible and integration-facing changes here.

## 0.1.0-alpha.1

- Extract the clean-room Audio Server Plug-in into a source-only standalone repository.
- Add bounded dynamic virtual input/output endpoints and allocation-free realtime transport.
- Add validated, centralized downstream product identity generation.
- Answer `kAudioObjectPropertyControlList` with an empty list. These endpoints carry no volume or
  mute control, but the host asks anyway and stops activating a device that refuses the question
  rather than answering "none" — so every device loaded, published, and was dropped, which from
  outside looked exactly like a driver that never loaded at all.
- Report why a stored catalog did not come back, at debug level, rather than starting empty in
  silence.
- Cover the host's activation walk and the output endpoint's realtime bridge with tests that replay
  what the host actually asks for, so a property the host needs cannot be dropped again without a
  failure.
