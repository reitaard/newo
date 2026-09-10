import unittest
from unittest.mock import Mock, patch

from newo_csi.discovery import ANNOUNCEMENT_PORT, MULTICAST_GROUP, CollectorAnnouncer


class AnnouncerTests(unittest.TestCase):
    @patch("newo_csi.discovery.socket.socket")
    def test_announcement_is_rate_limited_and_ttl_one(self, socket_factory):
        sock = Mock()
        socket_factory.return_value = sock
        announcer = CollectorAnnouncer(5005, interval=5.0, nonce=7)
        announcer.poll(10.0)
        announcer.poll(12.0)
        announcer.poll(15.0)
        self.assertEqual(sock.sendto.call_count, 2)
        self.assertEqual(sock.sendto.call_args.args[1], (MULTICAST_GROUP, ANNOUNCEMENT_PORT))
        announcer.close()
        sock.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
