"""Camera reports: what a camera has, for its user to send in.

A device's "camera_report" trigger asks the camera what it has, and the
"ptz_get_camera_report" proc hands back the report, as JSON, for the user
to look at and send in themselves (see doc/visca-protocol.md). Nothing in
it may identify the camera, its user, or where either is.
"""

from test_visca_profiles import camera


def test_there_is_no_report_until_one_is_made(request, obs_world, tmp_path):
    _, _, device_id = camera(request, obs_world, tmp_path)
    assert obs_world.camera_report(device_id, tmp_path / "report.json") is None
