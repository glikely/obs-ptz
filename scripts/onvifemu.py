#!/usr/bin/env python3
"""Standalone ONVIF camera emulator.

The actual implementation now lives in ptzsim.backends.onvif, so it can
be reused as one of ptzsim's own protocol backends. This script is a
thin wrapper preserving the original standalone entry point.
"""

from ptzsim.backends.onvif import main

if __name__ == "__main__":
    main()
