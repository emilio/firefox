import pytest
from webdriver.transport import HTTPWireProtocol


@pytest.fixture(name="configuration", scope="session")
def fixture_configuration(configuration):
    """Remove "acceptInsecureCerts" from capabilities if it exists.

    Some browser configurations add acceptInsecureCerts capability by default.
    Remove it during new_session tests to avoid interference.
    """
    if "acceptInsecureCerts" in configuration["capabilities"]:
        configuration = dict(configuration)
        del configuration["capabilities"]["acceptInsecureCerts"]
    return configuration


@pytest.fixture(name="new_session")
def fixture_new_session(request, configuration, current_session):
    """Start a new session for tests which themselves test creating new sessions.

    :param body: The content of the body for the new session POST request.

    :param delete_existing_session: Allows the fixture to delete an already
     created custom session before the new session is getting created. This
     is useful for tests which call this fixture multiple times within the
     same test.
    """
    custom_session = {}

    transport = HTTPWireProtocol(
        configuration["host"],
        configuration["port"],
        url_prefix="/",
    )

    def _delete_session(session_id):
        transport.send("DELETE", f"session/{session_id}")

    def new_session(body, delete_existing_session=False, headers=None):
        # If there is an active session from the global session fixture,
        # delete that one first
        if current_session is not None:
            current_session.end()

        if delete_existing_session:
            _delete_session(custom_session["session"]["sessionId"])

        response = transport.send("POST", "session", body, headers=headers)
        if response.status == 200:
            custom_session["session"] = response.body["value"]
        return response, custom_session.get("session", None)

    yield new_session

    if custom_session.get("session") is not None:
        _delete_session(custom_session["session"]["sessionId"])
        custom_session = None
