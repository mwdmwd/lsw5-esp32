# SPDX-License-Identifier: GPL-3.0-or-later
"""Small, unprivileged checks for the policy's access boundaries."""

import ipaddress
import unittest

import hotspot as lab


class PolicyTests(unittest.TestCase):
    def parse(self, text):
        return lab.read_policy(text, [ipaddress.IPv4Network(n) for n in lab.SPECIAL])

    def test_dns_alone_does_not_open_connections(self):
        policy = self.parse("dns-pass api.example.com")
        self.assertEqual(policy["allowed"], {"tcp": [], "udp": []})
        self.assertEqual(policy["resolved"], {})

    def test_exact_dns_and_port_tuples(self):
        policy = self.parse("allow tcp API.example.com 443 10000\ndns api.example.com 9.9.9.9")
        self.assertEqual(policy["allowed"]["tcp"], [("9.9.9.9", 443), ("9.9.9.9", 10000)])
        self.assertEqual(policy["allowed"]["udp"], [])

    def test_rejects_local_destinations_and_external_dns(self):
        for rule in (
            "allow tcp 192.168.1.1 443",
            "allow tcp 127.0.0.1 443",
            "allow udp 224.0.0.1 123",
            "allow udp 9.9.9.9 53",
        ):
            with self.subTest(rule=rule), self.assertRaises(ValueError):
                self.parse(rule)

    def test_output_allow_is_separate_and_limited_to_hotspot_clients(self):
        policy = self.parse("output-allow tcp 10.77.0.115 80 443\noutput-allow udp 10.77.0.115 123")
        self.assertEqual(
            policy["output_allowed"],
            {
                "tcp": [("10.77.0.115", 80), ("10.77.0.115", 443)],
                "udp": [("10.77.0.115", 123)],
            },
        )
        self.assertEqual(policy["allowed"], {"tcp": [], "udp": []})
        for destination in ("10.77.0.0", "10.77.0.1", "10.77.0.255", "192.168.1.1", "9.9.9.9"):
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                self.parse(f"output-allow tcp {destination} 80")

    def test_intercept_does_not_allow_forwarding_or_direct_pc_access(self):
        policy = self.parse(
            "dns updates.example.com 9.9.9.9\nintercept tcp updates.example.com 443 8443"
        )
        self.assertEqual(policy["intercept_tcp"], [("9.9.9.9", 443, 8443)])
        self.assertEqual(policy["allowed"], {"tcp": [], "udp": []})
        self.assertEqual(policy["output_allowed"], {"tcp": [], "udp": []})

    def test_intercept_rejects_private_targets_and_direct_bypass(self):
        for rules in (
            "intercept tcp 192.168.1.1 443 8443",
            "intercept tcp 9.9.9.9 53 8443",
            "intercept tcp 9.9.9.9 443 22",
            "allow tcp 9.9.9.9 443\nintercept tcp 9.9.9.9 443 8443",
            "intercept tcp 9.9.9.9 443 8443\nintercept tcp 9.9.9.9 443 8444",
        ):
            with self.subTest(rules=rules), self.assertRaises(ValueError):
                self.parse(rules)


if __name__ == "__main__":
    unittest.main()
