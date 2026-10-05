# Camera reports

Camera reports sent in on GitHub (docs/visca-protocol.md, "Camera Reports"),
one file each, named for the camera: `sony-srg-300h.json`, say.
ptzsim replays each one as the camera it was made of (`--visca-report`), and
`tests/obs-integration/test_camera_report_replay.py` checks that the command
set the plugin chooses for that camera asks it for nothing it doesn't have.

Add a report as it was sent in, from its issue. JSON has no comments, so say
which issue it came from in the commit that adds it.

`ptzsim-birddog-p100.json` isn't from a camera: the plugin made it of ptzsim
imitating a BirdDog P100, so there is one to test with until reports from real
cameras come in.
