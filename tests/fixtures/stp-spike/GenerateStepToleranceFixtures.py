#!/usr/bin/env python3
"""Generate the T07 STEP tolerance fixtures from the committed part fixture.

The source is tests/fixtures/stp-spike/part_ap214.stp (generated once by the
OCCT XDE writer via GenerateStepFixtures.cpp and frozen as the durable
artifact). These derived files only change header/unit declarations, so they
stay small, clear-text, and byte-reproducible without linking the CAD kernel.

Run from this directory:  python GenerateStepToleranceFixtures.py
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "part_ap214.stp"


def read_source() -> str:
    return SOURCE.read_text(encoding="ascii")


def write(name: str, text: str) -> None:
    (HERE / name).write_text(text, encoding="ascii", newline="\n")
    print(f"wrote {name} ({len(text.encode('ascii'))} bytes)")


def external_document_self_contained(source: str) -> str:
    """Main document keeps its own geometry but declares an unresolved child."""
    source = source.replace(
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\n",
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\n"
        "FILE_POPULATION(('AP214'),('AP214IS'),($),$);\n",
        1,
    )
    source = source.replace(
        "ENDSEC;\nEND-ISO-10303-21;\n",
        "#440 = DOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'other part',#1);\n"
        "ENDSEC;\nEND-ISO-10303-21;\n",
        1,
    )
    return source


def no_unit(source: str) -> str:
    """Remove every authored length-unit entity; angle/solid-angle stay."""
    updated = source.replace(
        "#424 = ( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) );\n",
        "",
        1,
    )
    updated = updated.replace(
        "#427 = UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07),#424,\n"
        "  'distance_accuracy_value','confusion accuracy');\n",
        "",
        1,
    )
    updated = updated.replace(
        "GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((#427)) GLOBAL_UNIT_ASSIGNED_CONTEXT\n"
        "((#424,#425,#426)) REPRESENTATION_CONTEXT",
        "GLOBAL_UNIT_ASSIGNED_CONTEXT\n"
        "((#425,#426)) REPRESENTATION_CONTEXT",
        1,
    )
    if "#424 =" in updated or "#427 =" in updated or "#424," in updated:
        raise SystemExit("no-unit transform did not match the source")
    return updated


def contradictory_units(source: str) -> str:
    """Two distinct length units; millimetre is first (and therefore chosen)."""
    updated = source.replace(
        "((#424,#425,#426)) REPRESENTATION_CONTEXT",
        "((#424,#440,#425,#426)) REPRESENTATION_CONTEXT",
        1,
    )
    if updated == source:
        raise SystemExit("contradictory-unit transform did not match the source")
    updated = updated.replace(
        "ENDSEC;\nEND-ISO-10303-21;\n",
        "#440 = ( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT($,.METRE.) );\n"
        "ENDSEC;\nEND-ISO-10303-21;\n",
        1,
    )
    return updated


def repair_needed(source: str) -> str:
    """One transferable box face with an empty edge loop.

    The main document transfers, no definition tessellates, and the bounded
    repair attempt cannot make an empty wire valid. The host must therefore
    preserve its typed EmptyGeometry outcome rather than publish a fabricated
    scene. This is the smallest deterministic 'repair attempted, impossible'
    fixture that does not mutate valid geometry.
    """
    box = (HERE / "face_color_ap214.stp").read_text(encoding="ascii")
    box = box.replace(
        "#16 = CLOSED_SHELL('',(#17,#137,#237,#284,#331,#338));",
        "#16 = CLOSED_SHELL('',(#17));",
        1,
    )
    box = box.replace(
        "#19 = EDGE_LOOP('',(#20,#55,#83,#111));",
        "#19 = EDGE_LOOP('',());",
        1,
    )
    return box


def unrepairable(source: str) -> str:
    """A missing ADVANCED_FACE entity cannot be reconstructed by ShapeFix."""
    updated = source.replace("#17 = ADVANCED_FACE('',(#18),#32,.F.);\n", "", 1)
    if updated == source:
        raise SystemExit("unrepairable transform did not match the source")
    return updated


def main() -> None:
    source = read_source()
    write("external_document_self_contained_ap214.stp",
          external_document_self_contained(source))
    write("no_unit_ap214.stp", no_unit(source))
    write("contradictory_units_ap214.stp", contradictory_units(source))
    write("repair_needed_ap214.stp", repair_needed(source))
    write("unrepairable_ap214.stp", unrepairable(source))


if __name__ == "__main__":
    main()
