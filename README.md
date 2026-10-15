# BOM Tool
Tool for generating BOM and CPL files from KiCad PCB (.kicad_pcb) files.

## Features
* Support for KiCad 10
* Zips gerber files
* Generates generic BOM
* Generate BOM and CPL files for JLCPCB
* Basic support for variants (do not place attribute)

## Usage

$ bom-tool \<options> \<path to .kicad_pcb file> \<output directory>

Option | Description
-------|-------------
-n     | Name for output files (optional, derived from .kicad_pcb file name if not given)
-v     | Variant to use (optional)
-g     | Export and zip gerber files (path to gerber and layers are read from the .kicad_pcb file)
-b     | Generate BOM and placement file
-j     | Generate for JLCPCB (oval holes alternate, BOM with LCSC PN, CPL file)

Multiple .kicad_pcb files can be processed at once. This example zips the gerber for both onlyPcb.kicad_pcb and pcbAndBom.kicad_pcb and generats BOM files for pcbAndBom.kicad_pcb:

```console
$ bom-tool -j -g onlyPcb.kicad_pcb -g -b pcbAndBom.kicad_pcb /path/to/output/directory
```

## Build/Develop

Use [conan](README-conan.md)
