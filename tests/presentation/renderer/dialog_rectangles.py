"""Geometry assertions shared by the lobby and front-end runtime cases."""
import itertools


def assert_dialog_rectangles(test, geometry, viewport=None):
    frame = geometry["dialog"]
    controls = geometry["controls"]
    test.assertTrue(controls, "the loaded dialog must expose visible controls")

    def contains(outer, inner):
        x, y, w, h = outer
        a, b, c, d = inner
        return x - 0.5 <= a and y - 0.5 <= b and a + c <= x + w + 0.5 and b + d <= y + h + 0.5

    if viewport is not None:
        test.assertTrue(contains([0, 0, *viewport], frame), ("dialog outside viewport", frame, viewport))
    for control in controls:
        test.assertTrue(contains(frame, control["rect"]), ("outside dialog", control, frame))
    for first, second in itertools.combinations(controls, 2):
        x, y, w, h = first["rect"]
        a, b, c, d = second["rect"]
        if min(x + w, a + c) - max(x, a) <= 0.5 or min(y + h, b + d) - max(y, b) <= 0.5:
            continue
        # The connection details are a deliberate overlay over staging, opened on demand.
        if "EAWR_ONLINE_CONNECTIONS_LIST" in {first["id"], second["id"]}:
            continue
        # Authored group frames intentionally surround their controls and headings.
        if any(item["kind"] == "group" and contains(item["rect"], other["rect"])
               for item, other in ((first, second), (second, first))):
            continue
        # The loaded NAT caption starts two resource units left of the firewall
        # backdrop. It must keep its authored rectangle; the frame is not an input.
        if any(item["id"] == "IDC_STATIC" and item["kind"] == "group"
               and other["id"] == "IDC_TEXT_ALLOW_NAT_NEGOTIATION"
               and other["rect"] == other.get("authored")
               and contains(item["rect"], [other["rect"][0] + other["rect"][2] / 2,
                                           other["rect"][1] + other["rect"][3] / 2, 0, 0])
               for item, other in ((first, second), (second, first))):
            continue
        ids = {first["id"], second["id"]}
        # Retail embeds the chat edit over the list's lower border (six resource units).
        if ids == {"IDC_LAN_CHAT_LIST", "IDC_LAN_CHAT_EDIT"}:
            continue
        # The empty/unavailable notice intentionally sits inside the room list.
        if ids == {"IDC_GAME_LIST", "EAWR_LOBBY_ONLINE_NOTICE"}:
            continue
        test.fail(("overlapping controls", first, second))
