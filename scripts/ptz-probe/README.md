# ptz-probe

Makes an obs-ptz camera report of a VISCA camera without OBS, for anyone
who can't run the plugin, or whose camera the plugin can't talk to at all.
It asks the camera what the plugin's own **Create Camera Report…** does
(see "Camera Reports" in `doc/visca-protocol.md`), and writes the same
report, with `"made_by": "ptz-probe"`, for its user to look over and send in
with the camera report form on GitHub.

It talks to the camera and to nothing else, and never sends the report
anywhere. It doesn't move the camera: it asks for everything the plugin can
ask any camera for, then sends each value the camera can set back to it as
the camera said it was, which changes nothing. It doesn't ask a camera in
standby that can't be asked for everything then.

`ptz-probe.py` is one file, which needs only Python 3.8 or later:

```
python3 ptz-probe.py 192.168.0.20                  # VISCA over IP (UDP, port 52381)
python3 ptz-probe.py 192.168.0.20 --tcp [PORT]     # VISCA over TCP (port 5678)
python3 ptz-probe.py 192.168.0.20 --dvip [PORT]    # Datavideo DVIP (port 5002)
python3 ptz-probe.py /dev/ttyUSB0 --serial [--baud 9600] [--address 1]   # Linux, macOS
```

It writes `camera-report.json`, or the file `-o` names. Quit OBS, or
anything else talking to the camera, first: a Sony answers VISCA over IP on
UDP port 52381, whatever port it was asked from.

What it asks for, and how it reads and writes the values, it has from the
plugin's source, in tables at its top that `gen_tables.py` writes there:
the generic command set and the vendor and model names in
`src/ptz-visca-commands.cpp`, the command sets in `src/visca-profiles`, and
the plugin's release version. Run `python3 scripts/ptz-probe/gen_tables.py` after
changing any of them; CI runs it with `--check`, which fails if the tables
aren't what it would write.

`tests/obs-integration/test_ptz_probe.py` runs it against ptzsim, every way
there is to a camera, and checks that it reports what the plugin's own
report of the same camera does.

Its report's `plugin_version` is `git describe --dirty`, as the plugin's is, when
it is run from a checkout of the plugin, and the release its tables were written
for when it is run on its own.
