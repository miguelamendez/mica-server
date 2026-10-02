#!/usr/bin/env python3
"""Dependency-free tests for playground API-key handling."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "mica_playground", ROOT / "apps/mica_playground.py"
)
PLAYGROUND = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PLAYGROUND)


class PlaygroundAuthTests(unittest.TestCase):
    @staticmethod
    def handler(key: str = "", fallback: str = ""):
        handler = object.__new__(PLAYGROUND.Handler)
        handler.headers = {"X-Mica-API-Key": key} if key else {}
        handler.server = SimpleNamespace(api_key=fallback)
        return handler

    def test_missing_key_returns_explicit_401(self) -> None:
        handler = self.handler()
        sent = []
        handler._send = lambda status, content_type, body: sent.append(
            (status, content_type, body)
        )
        self.assertIsNone(handler._authorization_headers())
        self.assertEqual(sent[0][0], 401)
        self.assertIn(b'"code": "api_key_required"', sent[0][2])

    def test_tab_key_is_forwarded_as_bearer_token(self) -> None:
        handler = self.handler("tab-token-0123456789", "fallback-token")
        headers = handler._authorization_headers("application/json")
        self.assertEqual(headers["Authorization"], "Bearer tab-token-0123456789")
        self.assertEqual(headers["Content-Type"], "application/json")

    def test_cli_key_precedes_config_and_key_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            key_file = Path(temporary) / "key"
            key_file.write_text("file-token\n")
            args = Namespace(api_key="cli-token", api_key_file=key_file)
            config = {"api_key": "config-token", "api_key_file": str(key_file)}
            self.assertEqual(PLAYGROUND.configured_key(args, config), "cli-token")

    def test_chat_exposes_local_appearance_and_prompt_controls(self) -> None:
        page = PLAYGROUND.CHAT_HTML
        for control in (
            'id="theme"',
            'id="logoUpload"',
            'id="logoReset"',
            'id="llmSystemPrompt"',
            'id="vlmSystemPrompt"',
            'id="resetPrompts"',
        ):
            self.assertIn(control, page)
        self.assertIn("f.append('llm_system_prompt'", page)
        self.assertIn("f.append('vlm_system_prompt'", page)
        self.assertIn("data-mica-logo", page)

    def test_markdown_renderer_escapes_html_and_rejects_unsafe_links(self) -> None:
        page = PLAYGROUND.CHAT_HTML
        self.assertIn("const escapeHtml=", page)
        self.assertIn("https?:\\/\\/|mailto:|\\/", page)
        self.assertIn("host.querySelector('img')", page)
        self.assertIn("host.querySelectorAll('a').length!==1", page)


if __name__ == "__main__":
    unittest.main()
