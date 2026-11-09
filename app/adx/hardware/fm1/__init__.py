"""The M-Vave FM-1 (phase_11.5.md): its profiles, identity, the connected device, its
firmware updater and recovery detection.

Everything here runs on the main thread. The engine moves the bytes (SysEx in through
``Engine.drain_sysex``, out through ``Engine.send_sysex``); nothing here touches audio.
"""
