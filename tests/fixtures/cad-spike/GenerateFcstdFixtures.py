#!/usr/bin/env python3
"""T09 FreeCAD Standard fixture generator.

Builds the small `.FCStd` packages from the repository's own `part.brep`
fixture. No third-party FreeCAD document is copied in. The committed files are
durable; run this script from the repository root after changing the XML model:

    python tests/fixtures/cad-spike/GenerateFcstdFixtures.py
"""

from __future__ import annotations

import pathlib
import struct
import zipfile

HERE = pathlib.Path(__file__).resolve().parent

DOCUMENT = """<?xml version='1.0' encoding='utf-8'?>
<Document SchemaVersion="4" ProgramVersion="1.0R20240501" FileVersion="1">
  <Properties Count="1">
    <Property name="Label" type="App::PropertyString"><String value="T09 PartDesign"/></Property>
  </Properties>
  <Objects Count="3">
    <Object type="App::Part" name="Assembly"/>
    <Object type="PartDesign::Body" name="Body"/>
    <Object type="Part::Feature" name="HiddenBase"/>
  </Objects>
  <ObjectData Count="3">
    <Object name="Assembly">
      <Properties Count="3">
        <Property name="Label" type="App::PropertyString"><String value="Assembly"/></Property>
        <Property name="Placement" type="App::PropertyPlacement">
          <PropertyPlacement Px="10.0" Py="0.0" Pz="0.0" Q0="0.0" Q1="0.0" Q2="0.0" Q3="1.0" A="0" Ox="0" Oy="0" Oz="1"/>
        </Property>
        <Property name="Group" type="App::PropertyLinkList">
          <LinkList count="1"><Link value="Body"/></LinkList>
        </Property>
      </Properties>
    </Object>
    <Object name="Body">
      <Properties Count="4">
        <Property name="Label" type="App::PropertyString"><String value="Body"/></Property>
        <Property name="Shape" type="Part::PropertyPartShape"><Part file="PartShape.brp"/></Property>
        <Property name="Placement" type="App::PropertyPlacement">
          <PropertyPlacement Px="5.0" Py="0.0" Pz="0.0" Q0="0.0" Q1="0.0" Q2="0.0" Q3="1.0" A="0" Ox="0" Oy="0" Oz="1"/>
        </Property>
        <Property name="Visibility" type="App::PropertyBool"><Bool value="true"/></Property>
      </Properties>
    </Object>
    <Object name="HiddenBase">
      <Properties Count="3">
        <Property name="Shape" type="Part::PropertyPartShape"><Part file="HiddenShape.brp"/></Property>
        <Property name="Placement" type="App::PropertyPlacement">
          <PropertyPlacement Px="0.0" Py="0.0" Pz="0.0" Q0="0.0" Q1="0.0" Q2="0.0" Q3="1.0" A="0" Ox="0" Oy="0" Oz="1"/>
        </Property>
        <Property name="Visibility" type="App::PropertyBool"><Bool value="false"/></Property>
      </Properties>
    </Object>
  </ObjectData>
</Document>
"""

GUI_DOCUMENT = """<?xml version='1.0' encoding='utf-8'?>
<Document SchemaVersion="1">
  <ViewProviderData Count="1">
    <ViewProvider name="Body" expanded="0">
      <Properties Count="2">
        <Property name="ShapeColor" type="App::PropertyColor"><PropertyColor value="13369344"/></Property>
        <Property name="Transparency" type="App::PropertyPercent"><Integer value="20"/></Property>
      </Properties>
    </ViewProvider>
  </ViewProviderData>
</Document>
"""

EXTERNAL_LINK_DOCUMENT = DOCUMENT.replace(
    '<Object type="Part::Feature" name="HiddenBase"/>',
    '<Object type="Part::Feature" name="HiddenBase"/>\n    <Object type="App::Link" name="External"/>',
).replace(
    '<Object name="HiddenBase">',
    '<Object name="External">\n'
    '      <Properties Count="1">\n'
    '        <Property name="LinkedObject" type="App::PropertyXLink">\n'
    '          <XLink name="FarAway" file="OtherDocument.FCStd"/>\n'
    '        </Property>\n'
    '      </Properties>\n'
    '    </Object>\n'
    '    <Object name="HiddenBase">',
)


def write_package(name: str, document: str, extra: dict[str, bytes]) -> None:
    path = HERE / name
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("Document.xml", document.encode("utf-8"))
        archive.writestr("GuiDocument.xml", GUI_DOCUMENT.encode("utf-8"))
        for entry_name, payload in extra.items():
            archive.writestr(entry_name, payload)
    print(f"wrote {path}")


def main() -> int:
    shape = (HERE / "part.brep").read_bytes()
    write_package(
        "partdesign.FCStd",
        DOCUMENT,
        {"PartShape.brp": shape, "HiddenShape.brp": shape},
    )
    write_package(
        "external_link.FCStd",
        EXTERNAL_LINK_DOCUMENT,
        {"PartShape.brp": shape, "HiddenShape.brp": shape},
    )
    # Well-formed ZIP with malformed Document.xml.
    write_package("malformed.FCStd", "<Document><Objects>", {"PartShape.brp": shape})
    # Well-formed ZIP missing the required FreeCAD document part.
    with zipfile.ZipFile(HERE / "nodocument.FCStd", "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("PartShape.brp", shape)
    print(f"wrote {HERE / 'nodocument.FCStd'}")
    # Deflate bomb: 128 MiB of zeros compresses to well under the source cap but
    # exceeds the documented expansion-ratio policy before extraction.
    write_package("zipbomb.FCStd", DOCUMENT, {"PartShape.brp": b"\0" * (128 * 1024 * 1024)})

    # A structurally valid directory that claims a 300 MiB expanded entry. The
    # scanner rejects EntryTooLarge from the central directory and never reads
    # the (absent) data, so this file stays tiny.
    declared = 300 * 1024 * 1024
    name = b"PartShape.brp"
    local = (
        struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0, 0, declared, declared,
                    len(name), 0)
        + name
    )
    central = (
        struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0, 0, declared,
                    declared, len(name), 0, 0, 0, 0, 0, 0)
        + name
    )
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1, len(central), len(local), 0)
    (HERE / "oversized.FCStd").write_bytes(local + central + eocd)
    print(f"wrote {HERE / 'oversized.FCStd'}")

    # Truncate the positive package mid-archive.
    positive = (HERE / "partdesign.FCStd").read_bytes()
    (HERE / "truncated.FCStd").write_bytes(positive[: len(positive) // 3])
    print(f"wrote {HERE / 'truncated.FCStd'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
