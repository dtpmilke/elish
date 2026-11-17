#!/usr/bin/env python3
"""Generate the qcdx panel configuration from the elish K81 42 02 0a UEFI XML."""
import argparse
import hashlib
import re
from pathlib import Path

SOURCE_SHA256 = "62aa7bd9b434c7622da7b3bb1709c69c1962a9a3ea026c2fea25602b557f480f"
OUTPUT_SHA256 = "0337bebc6fbf8359181e5b7fc33a4076064105f8d86b532145bd7ab241ee1641"

EDID = """<Group id='EDID Configuration'>
    <ManufactureID>0x61A9</ManufactureID>
    <ProductCode>0x8142</ProductCode>
    <SerialNumber>0x000001</SerialNumber>
    <WeekofManufacture>0x01</WeekofManufacture>
    <YearofManufacture>0x1F</YearofManufacture>
    <EDIDVersion>1</EDIDVersion>
    <EDIDRevision>3</EDIDRevision>
    <VideoInputDefinition>0x80</VideoInputDefinition>
    <HorizontalScreenSize>0x0F</HorizontalScreenSize>
    <VerticalScreenSize>0x18</VerticalScreenSize>
    <DisplayTransferCharacteristics>0x78</DisplayTransferCharacteristics>
    <FeatureSupport>0x2</FeatureSupport>
    <Red.GreenBits>0xA5</Red.GreenBits>
    <Blue.WhiteBits>0x58</Blue.WhiteBits>
    <RedX>0xA6</RedX>
    <RedY>0x54</RedY>
    <GreenX>0x33</GreenX>
    <GreenY>0xB3</GreenY>
    <BlueX>0x26</BlueX>
    <BlueY>0x12</BlueY>
    <WhiteX>0x4F</WhiteX>
    <WhiteY>0x54</WhiteY>
    <EstablishedTimingsI>0x0</EstablishedTimingsI>
    <EstablishedTimingsII>0x0</EstablishedTimingsII>
    <ManufacturesTiming>0x0</ManufacturesTiming>
    <StandardTimings1/>
    <StandardTimings2/>
    <StandardTimings3/>
    <StandardTimings4/>
    <StandardTimings5/>
    <StandardTimings6/>
    <StandardTimings7/>
    <SignalTimingInterface/>
</Group>
<Group id='Detailed Timing'>
    <HorizontalScreenSizeMM>0x94</HorizontalScreenSizeMM>
    <VerticalScreenSizeMM>0xED</VerticalScreenSizeMM>
    <HorizontalVerticalScreenSizeMM>0x00</HorizontalVerticalScreenSizeMM>
</Group>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="original UEFI panel XML")
    parser.add_argument("output", type=Path, help="new qcdx panel configuration")
    args = parser.parse_args()

    source = args.source.read_text(encoding="ascii")
    source_hash = hashlib.sha256(source.encode("ascii")).hexdigest()
    if source_hash != SOURCE_SHA256:
        raise SystemExit(f"error: input SHA-256 is {source_hash}; expected {SOURCE_SHA256}")

    panel = source.replace('\\"', '"')
    panel = re.sub(r"^<\?xml[^>]*\?>\s*", "", panel)
    description = re.search(r"<PanelDescription>.*?</PanelDescription>\s*", panel)
    panel = panel[:description.start()] + panel[description.end():]
    output = (
        "<?xml version='1.0' encoding='utf-8'?>\n"
        "<PanelName>K81_42_02_0A</PanelName>\n"
        f"{description.group().strip()}\n{EDID}{panel.strip()}\n"
    ).encode("ascii")
    output_hash = hashlib.sha256(output).hexdigest()
    if output_hash != OUTPUT_SHA256:
        raise SystemExit(f"error: output SHA-256 is {output_hash}; expected {OUTPUT_SHA256}")
    with args.output.open("xb") as file:
        file.write(output)
    print(f"{args.output}: {len(output)} bytes, SHA-256 {output_hash}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, UnicodeError) as exc:
        raise SystemExit(f"error: {exc}") from None
