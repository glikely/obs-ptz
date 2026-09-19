#!/usr/bin/env python3
"""Standalone VISCA-over-TCP camera emulator.

The actual implementation now lives in ptzsim.backends.visca, so it can
be reused as one of ptzsim's own protocol backends. This script is a
thin wrapper preserving the original standalone entry point.
"""

from ptzsim.backends.visca import main

if __name__ == "__main__":
    main()
