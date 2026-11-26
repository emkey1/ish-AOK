#!/usr/bin/env python3
"""Read AOK-iSH's TestFlight feedback through the App Store Connect API.

    tools/testflight-feedback.py                  every report, newest first
    tools/testflight-feedback.py --build 565      reports on build 565 and later
    tools/testflight-feedback.py --since 2026-11-20
    tools/testflight-feedback.py --build 565 --save build/testflight-feedback

Lists both kinds of report: feedback a tester sent with a screenshot, and
the comment a tester left when TestFlight offered to send a crash. A count per
build comes first, then each report with its build, device, iOS version and
comment. --save also writes each report's screenshots and crash log, and its
details as JSON, to a directory (build/ is ignored by git).

Run it in the release triage, beside the Organizer. Crash comments are only
here: the Organizer's crash download never has them. A crash with no comment
is in the Organizer too, symbolicated, so read its log there; this one's log
is the raw report.

Credentials, all outside the repository:
    the API key     ~/.appstoreconnect/private_keys/AuthKey_<KEY_ID>.p8
                    (the only one there, or set ASC_KEY_ID)
    the issuer ID   ~/.appstoreconnect/issuer_id, or ASC_ISSUER_ID; it is on
                    App Store Connect under Users and Access > Integrations >
                    App Store Connect API

Read only: nothing here changes App Store Connect. The tester's email address
is in the API's answer and is left out of the listing and the saved JSON.
Tokens are signed with openssl, since macOS's Python has no crypto module.
"""
import argparse
import base64
import glob
import json
import os
import subprocess
import sys
import textwrap
import time
import urllib.error
import urllib.request

API = "https://api.appstoreconnect.apple.com"
DEFAULT_BUNDLE = "app.ish.iSH-AOK"
KEY_DIR = os.path.expanduser("~/.appstoreconnect/private_keys")
ISSUER_FILE = os.path.expanduser("~/.appstoreconnect/issuer_id")
KINDS = (
    ("betaFeedbackScreenshotSubmissions", "feedback"),
    ("betaFeedbackCrashSubmissions", "crash"),
)


def die(message):
    sys.exit(f"{os.path.basename(sys.argv[0])}: {message}")


def credentials():
    key_id = os.environ.get("ASC_KEY_ID")
    if key_id:
        key = os.path.join(KEY_DIR, f"AuthKey_{key_id}.p8")
        if not os.path.exists(key):
            die(f"no {key}")
    else:
        keys = sorted(glob.glob(os.path.join(KEY_DIR, "AuthKey_*.p8")))
        if len(keys) != 1:
            die(f"found {len(keys)} keys in {KEY_DIR}; set ASC_KEY_ID to pick one" if keys
                else f"no API key in {KEY_DIR} (AuthKey_<KEY_ID>.p8)")
        key = keys[0]
        key_id = os.path.basename(key)[len("AuthKey_"):-len(".p8")]
    issuer = os.environ.get("ASC_ISSUER_ID")
    if not issuer and os.path.exists(ISSUER_FILE):
        issuer = open(ISSUER_FILE).read().strip()
    if not issuer:
        die(f"no issuer ID: put it in {ISSUER_FILE} or set ASC_ISSUER_ID "
            "(App Store Connect > Users and Access > Integrations > App Store Connect API)")
    return key_id, key, issuer


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")


def der_to_raw(der):
    # An ECDSA signature from openssl is DER, SEQUENCE { INTEGER r, INTEGER s };
    # a JWT wants the two 32-byte numbers side by side.
    if der[0] != 0x30:
        die("openssl's signature is not DER")
    i = 2 if der[1] < 0x80 else 2 + (der[1] & 0x7F)
    out = b""
    for _ in range(2):
        n = der[i + 1]
        out += der[i + 2:i + 2 + n].lstrip(b"\x00").rjust(32, b"\x00")
        i += 2 + n
    return out


class Client:
    def __init__(self):
        self.key_id, self.key, self.issuer = credentials()
        self._token, self._expires = "", 0

    def token(self):
        now = int(time.time())
        if now < self._expires - 60:
            return self._token
        head = b64(json.dumps({"alg": "ES256", "kid": self.key_id, "typ": "JWT"}).encode())
        body = b64(json.dumps({"iss": self.issuer, "iat": now, "exp": now + 1100,
                               "aud": "appstoreconnect-v1"}).encode())
        message = head + b"." + body
        signed = subprocess.run(["openssl", "dgst", "-sha256", "-sign", self.key],
                                input=message, capture_output=True)
        if signed.returncode != 0:
            die("openssl could not sign the token: " + signed.stderr.decode(errors="replace").strip())
        self._token = (message + b"." + b64(der_to_raw(signed.stdout))).decode()
        self._expires = now + 1100
        return self._token

    def get(self, path, missing_ok=False):
        url = path if path.startswith("http") else API + path
        request = urllib.request.Request(url)
        request.add_header("Authorization", "Bearer " + self.token())
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as e:
            if e.code == 404 and missing_ok:
                return None
            detail = e.read().decode(errors="replace")
            try:
                errors = json.loads(detail).get("errors", [])
                detail = "; ".join(f"{x.get('code')}: {x.get('detail') or x.get('title')}"
                                   for x in errors) or detail
            except ValueError:
                pass
            die(f"GET {url.replace(API, '')}: HTTP {e.code}: {detail}")
        except urllib.error.URLError as e:
            die(f"GET {url.replace(API, '')}: {e.reason}")

    def get_all(self, path):
        data, included = [], []
        while path:
            page = self.get(path)
            data += page.get("data", [])
            included += page.get("included", [])
            path = page.get("links", {}).get("next")
        return data, included


def build_number(version):
    try:
        return int(version)
    except (TypeError, ValueError):
        return -1


def fetch(client, bundle):
    apps = client.get(f"/v1/apps?filter[bundleId]={bundle}&fields[apps]=bundleId,name").get("data", [])
    apps = [a for a in apps if a["attributes"].get("bundleId") == bundle]
    if not apps:
        die(f"no app with bundle ID {bundle} on this account")
    app = apps[0]["id"]
    reports = []
    for endpoint, kind in KINDS:
        # The tester is asked for by id only; the API would otherwise add their
        # name and email to the answer.
        data, included = client.get_all(
            f"/v1/apps/{app}/{endpoint}?limit=200&include=build,tester"
            "&fields[builds]=version&fields[betaTesters]=inviteType")
        builds = {i["id"]: i["attributes"].get("version")
                  for i in included if i["type"] == "builds"}
        for item in data:
            attributes = dict(item["attributes"])
            attributes.pop("email", None)
            relations = item.get("relationships", {})
            build = (relations.get("build", {}).get("data") or {}).get("id")
            tester = (relations.get("tester", {}).get("data") or {}).get("id") or ""
            reports.append({
                "id": item["id"],
                "kind": kind,
                "build": builds.get(build) or "?",
                "tester": tester[:6],
                "attributes": attributes,
            })
    reports.sort(key=lambda r: r["attributes"].get("createdDate") or "", reverse=True)
    return reports


def device(attributes):
    # "iPhone17_5" is how the API spells the iPhone17,5 crash logs show.
    model = (attributes.get("deviceModel") or "?").replace("_", ",")
    platform = {"IOS": "iOS", "MAC_OS": "macOS", "VISION_OS": "visionOS"}.get(
        attributes.get("devicePlatform"), attributes.get("devicePlatform") or "")
    return f"{model}  {platform} {attributes.get('osVersion') or '?'}".strip()


def save(client, report, directory):
    attributes = report["attributes"]
    stem = "{}-{}-{}-{}".format(
        (attributes.get("createdDate") or "")[:16].replace(":", ""),
        report["build"], report["kind"], report["id"][:10])
    written = []
    for n, shot in enumerate(attributes.get("screenshots") or []):
        path = os.path.join(directory, f"{stem}-{n}.png")
        try:
            urllib.request.urlretrieve(shot["url"], path)
            written.append(path)
        except (urllib.error.URLError, KeyError) as e:
            print(f"    screenshot {n} not downloaded: {e}", file=sys.stderr)
    if report["kind"] == "crash":
        # Some reports have no log (2026-11-09's on 564, a hang the tester
        # force-quit): the API answers 404 for those.
        log = client.get(f"/v1/betaFeedbackCrashSubmissions/{report['id']}/crashLog",
                         missing_ok=True) or {}
        text = ((log.get("data") or {}).get("attributes") or {}).get("logText")
        if not text:
            print("    (no crash log on App Store Connect)")
        else:
            path = os.path.join(directory, f"{stem}.crash")
            with open(path, "w") as f:
                f.write(text)
            written.append(path)
    path = os.path.join(directory, f"{stem}.json")
    with open(path, "w") as f:
        json.dump(report, f, indent=1)
    written.append(path)
    return written


def main():
    parser = argparse.ArgumentParser(
        description="List AOK-iSH's TestFlight feedback and crash comments (read only).")
    parser.add_argument("--build", type=int, metavar="N",
                        help="only reports on build N or later")
    parser.add_argument("--since", metavar="YYYY-MM-DD",
                        help="only reports sent on or after this date (UTC)")
    parser.add_argument("--save", metavar="DIR",
                        help="also write screenshots, crash logs and details to DIR")
    parser.add_argument("--bundle", default=DEFAULT_BUNDLE,
                        help=f"the app's bundle ID (default {DEFAULT_BUNDLE})")
    parser.add_argument("--json", action="store_true",
                        help="print the reports as JSON instead of a listing")
    options = parser.parse_args()

    client = Client()
    every = fetch(client, options.bundle)
    reports = [r for r in every
               if (options.build is None or build_number(r["build"]) >= options.build)
               and (options.since is None
                    or (r["attributes"].get("createdDate") or "") >= options.since)]

    if options.json:
        json.dump(reports, sys.stdout, indent=1)
        print()
        return

    counts = {}
    for r in reports:
        counts.setdefault(r["build"], {"feedback": 0, "crash": 0})[r["kind"]] += 1
    scope = []
    if options.build is not None:
        scope.append(f"build {options.build} and later")
    if options.since:
        scope.append(f"since {options.since}")
    print(f"{len(reports)} of {len(every)} reports for {options.bundle}"
          + (f" ({', '.join(scope)})" if scope else ""))
    for build in sorted(counts, key=build_number, reverse=True):
        c = counts[build]
        print(f"  build {build:>5}: {c['feedback']:3} feedback  {c['crash']:3} crash")
    if options.build is not None and str(options.build) not in counts:
        print(f"  build {options.build:>5}: none")

    if options.save:
        os.makedirs(options.save, exist_ok=True)
    for r in reports:
        a = r["attributes"]
        when = (a.get("createdDate") or "?")[:16].replace("T", " ")
        extras = []
        shots = len(a.get("screenshots") or [])
        if shots:
            extras.append(f"{shots} screenshot" + ("s" if shots != 1 else ""))
        if a.get("buildBundleId") and a["buildBundleId"] != options.bundle:
            extras.append(a["buildBundleId"])
        print()
        print(f"{when} UTC  {r['build']:>5}  {r['kind']:8} {device(a)}  tester {r['tester']}"
              + (f"  ({', '.join(extras)})" if extras else ""))
        comment = (a.get("comment") or "").strip()
        for paragraph in (comment.splitlines() if comment else ["(no comment)"]):
            for line in textwrap.wrap(paragraph, 96) or [""]:
                print(("    " + line).rstrip())
        if options.save:
            for path in save(client, r, options.save):
                print("    -> " + path)


if __name__ == "__main__":
    main()
