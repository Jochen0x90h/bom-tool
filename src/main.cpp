#include "kicad.hpp"
#include <nlohmann/json.hpp>
#include <libzippp/libzippp.h> // https://github.com/ctabin/libzippp
#include <iostream>
#include <fstream>
#include <filesystem>
#include <set>
#include <numbers>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace libzippp;
using std::numbers::pi;


/// @brief Simple vector class
/// @tparam T Element type (e.g. double)
template <typename T>
struct Vector2 {
    T x;
    T y;
};

template <typename T1, typename T2>
inline auto operator +(const Vector2<T1> &a, const Vector2<T2> &b) {
    return Vector2<decltype(a.x + b.x)>(a.x + b.x, a.y + b.y);
}

template <typename T1, typename T2>
inline auto operator -(const Vector2<T1> &a, const Vector2<T2> &b) {
    return Vector2<decltype(a.x - b.x)>(a.x - b.x, a.y - b.y);
}

using double2 = Vector2<double>;


// substitute variables in a string, e.g. "${VERSION}" with "1.0"
void substituteVariables(std::string &str, const std::map<std::string, std::string> &variables) {
    size_t pos = 0;
    while (true) {
        pos = str.find("${", pos);
        if (pos == std::string::npos)
            break;
        size_t endPos = str.find('}', pos);
        if (endPos == std::string::npos)
            break;
        std::string varName = str.substr(pos + 2, endPos - pos - 2);
        auto it = variables.find(varName);
        if (it != variables.end()) {
            str.replace(pos, endPos - pos + 1, it->second);
            pos += it->second.length();
        } else {
            pos = endPos + 1;
        }
    }
}

// extract type of a component, e.g. "R" from "R1"
std::string getType(std::string_view reference) {
    size_t i = 0;
    while (i < reference.length()) {
        char ch = reference[i];
        if (ch >= '0' && ch <= '9')
            break;
        ++i;
    }
    return std::string(reference.substr(0, i));
}

// manufacturer
enum class Manufacturer {
    GENERIC,
    JLCPCB
};

struct Job {
    // output file name (without extension)
    std::string name;

    // export and zip gerber files using kicad-cli
    bool gerber;

    // generate manufacturer specific BOM and placement file
    bool bom;
    Manufacturer manufacturer;

    // export drill for OpenSCAD (used for 3D model generation)
    bool drill;

    // path to .kicad_pcb file
    fs::path pcbPath;
};

struct BomKey {
    std::string type;
    std::string value;
    int voltage; // in mV
    std::string footprint;
    std::string manufacturer;
    std::string mpn;

    auto operator <=>(const BomKey& other) const noexcept = default;
};

struct BomValue {
    std::vector<std::string> references; // list of references (e.g. R1, R2, C1...)
    int padCount;
    bool throughHole;
    std::string description;
};

struct JlcBomKey {
    std::string type;
    std::string value;
    std::string footprintName;
    std::string lcscPn;

    auto operator <=>(const JlcBomKey& other) const noexcept = default;
};


/// @brief BOM Tool: Zip gerber files and create BOM and CPL files
///
/// Usage:
/// bom-tool <options> <path to .kicad_pcb file> <output directory>
/// Options:
///   -n Name for output files (optional, derived from .kicad_pcb file name if not given)
///   -g Export and zip gerber files (path to gerber and layers are read from the .kicad_pcb file)
///   -b Generate BOM and placement file
///   -j Generate for JLCPCB (oval holes alternate, BOM with LCSC PN, CPL)
///
/// Multiple pcb files can be processed in one go
int main(int argc, const char **argv) {
    if (argc < 2)
        return 1;

    // parse arguments
    std::string name;
    bool gerber = false;
    bool bom = false;
    bool drill = false;
    Manufacturer manufacturer = Manufacturer::GENERIC;
    std::list<Job> jobs;
    fs::path outDir;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-n") {
            // set name of current job
            ++i;
            name = argv[i];
        } else if (arg == "-g") {
            // zip gerber
            gerber = true;
        } else if (arg == "-b") {
            // create generic bom (includes voltage)
            bom = true;
        } else if (arg == "-j") {
            // JLCPCB
            manufacturer = Manufacturer::JLCPCB;
        } else if (arg == "-d") {
            // export drill
            drill = true;
        } else {
            if (gerber || bom || drill) {
                // argument is path to .kicad_pcb file: add job
                fs::path pcbPath = arg;
                if (name.empty())
                    name = pcbPath.stem().string();

                jobs.emplace_back(name, gerber, bom, manufacturer, drill, pcbPath);

                // clear
                name.clear();
                gerber = false;
                bom = false;
                drill = false;
            } else {
                // argument is output directory
                outDir = arg;
            }
        }
    }

    std::cout << "Output directory: " << outDir.string() << std::endl;

    bool error = false;
    for (auto &job : jobs) {
        const char *manufacturers[] = {"Generic", "JLCPCB"};
        std::cout << "*** " << job.name << " for " << manufacturers[int(job.manufacturer)] << " ***" << std::endl;

        // try to read project (.kicad_pro) file for variables
        std::map<std::string, std::string> variables;
        {
            fs::path projectPath = job.pcbPath;
            projectPath.replace_extension(".kicad_pro");
            std::ifstream is(projectPath.string());
            if (is.is_open()) {
                try {
                    json j = json::parse(is);
                    json vars = j.at("text_variables");
                    for (auto it = vars.begin(); it != vars.end(); ++it) {
                        std::string key = it.key();
                        std::string value = it.value();
                        variables[key] = value;
                    }
                } catch (std::exception &e) {
                }
            }
        }

        // read pcb (.kicad_pcb) file
        std::ifstream s(job.pcbPath.string());
        if (!s) {
            // error
            std::cout << "Error: Can't read file " << job.pcbPath.string() << std::endl;
            return 1;
        }
        kicad::Container file;
        kicad::readFile(s, file);
        s.close();
        if (file.elements.empty()) {
            // error
            std::cout << "Error: File is empty " << job.pcbPath.string() << std::endl;
            return 1;
        }

        // get last write time of pcb
        auto pcbTime = fs::last_write_time(job.pcbPath);

        // get version suffix for file names
        std::string version;
        {
            auto titleBlockContainer = file.find("title_block");
            if (titleBlockContainer) {
                auto revContainer = titleBlockContainer->find("rev");
                if (revContainer) {
                    version = '-';
                    version += revContainer->getString(0);
                    substituteVariables(version, variables);
                }
            }
        }

        // zip gerber directory
        if (job.gerber) {

            // get layers
            std::set<std::string> layers;
            {
                auto layerContainer = file.find("layers");
                if (layerContainer) {
                    for (auto layer : *layerContainer) {
                        layers.insert(layer->getString(0));
                    }
                }
            }

            // get gerber directory from pcb file (configured in the plot dialog)
            auto setup = file.find("setup");
            if (setup != nullptr) {
                auto plotParams = setup->find("pcbplotparams");
                if (plotParams != nullptr) {
                    // get gerber directory
                    auto gerberDir = fs::weakly_canonical(job.pcbPath.parent_path() / plotParams->findString("outputdirectory"));
                    if (fs::is_directory(gerberDir)) {
                        // get selected layers
                        auto selection = plotParams->findString("layerselection");
                        std::string selectedLayers;
                        uint32_t flags[4] = {};
                        int index = 0;
                        int length = selection.size();
                        for (int i = 2; i < length; ++i) {
                            char ch = selection[i];

                            // check for next filed
                            if (ch == '_') {
                                ++index;
                                if (index == 4)
                                    break;
                            }

                            int nibble = ch <= '9' ? ch - '0' : (ch - 'a' + 10);
                            flags[index] = (flags[index] << 4) | nibble;
                        }

                        if (index <= 2) {
                            // old format (KiCad 8)
                            static const char *layerNames[] = {
                                "F.Adhesive", "B.Adhesive", "F.Paste", "B.Paste",
                                "F.Silkscreen", "B.Silkscreen", "F.Mask", "B.Mask",
                                "User.Drawings", "User.Comments", "User.Eco1", "User.Eco2",
                                "Edge.Cuts", "Margin", "F.Courtyard", "B.Courtyard",
                                "F.Fab", "B.Fab", "User.1", "User.2",
                                "User.3", "User.4", "User.5", "User.6",
                                "User.7", "User.8", "User.9"
                            };

                            // copper layers
                            if ((flags[1] & 1) != 0 && layers.contains("F.Cu"))
                                selectedLayers += "F.Cu,";
                            for (int i = 1; i < 31; ++i) {
                                if ((flags[1] >> i) & 1) {
                                    std::string layer = "In" + std::to_string(i) + ".Cu";
                                    if (layers.contains(layer)) {
                                        selectedLayers += layer;
                                        selectedLayers += ',';
                                    }
                                }
                            }
                            if ((flags[1] & 0x80000000) && layers.contains("B.Cu"))
                                selectedLayers += "B.Cu,";

                            // other layers
                            for (int i = 0; i < 27; ++i) {
                                if ((flags[0] >> i) & 1) {
                                        //if (layers.contains(layerNames[i])) {
                                            selectedLayers += layerNames[i];
                                            selectedLayers += ',';
                                        //}
                                    }
                            }
                        } else {
                            // new format (KiCad 9)
                            static const char *layerNames[] = {
                                "F.Mask",
                                "B.Mask",
                                "F.Silkscreen",
                                "B.Silkscreen",
                                "F.Adhesive",
                                "B.Adhesive",
                                "F.Paste",
                                "B.Paste",
                                "User.Drawings",
                                "User.Comments",
                                "User.Eco1",
                                "User.Eco2",
                                "Edge.Cuts",
                                "Margin",
                                "F.Courtyard",
                                "B.Courtyard",
                                "F.Fab",
                                "B.Fab",
                                "",
                                "User.1",
                                "User.2",
                                "User.3",
                                "User.4",
                                "User.5",
                                "User.6",
                                "User.7",
                                "User.8",
                                "User.9"
                            };

                            // copper layers (... x In3.Cu x In2.Cu x In1.Cu x B.Cu x F.Cu)
                            if ((flags[3] & 1) != 0 && layers.contains("F.Cu"))
                                selectedLayers += "F.Cu,";
                            for (int i = 2; i < 32; ++i) {
                                if ((flags[3 - i / 16] >> (i * 2 & 31)) & 1) {
                                    std::string layer = "In" + std::to_string(i - 1) + ".Cu";
                                    if (layers.contains(layer)) {
                                        selectedLayers += layer;
                                        selectedLayers += ',';
                                    }
                                }
                            }
                            if ((flags[3] & 4) && layers.contains("B.Cu"))
                                selectedLayers += "B.Cu,";

                            // other layers
                            for (int i = 0; i < 28; ++i) {
                                if ((flags[3 - i / 16] >> (i * 2 & 31)) & 2) {
                                    selectedLayers += layerNames[i];
                                    selectedLayers += ',';
                                        //std::cout << i << std::endl;
                                    //}
                                }
                            }
                        }

                        // remove trailing ','
                        if (!selectedLayers.empty())
                            selectedLayers.resize(selectedLayers.size() - 1);

                        // export gerber
                        {
                            std::cout << "Export gerber" << std::endl;
                            // add --check-zones
                            std::string command = "kicad-cli pcb export gerbers -l " + selectedLayers + " --subtract-soldermask --output " + gerberDir.string() + ' ' + job.pcbPath.string();
                            int result = std::system(command.c_str());
                            if (result != 0) {
                                error = true;
                                std::cerr << "Error: Gerber export, kicad-cli returned result " << result << std::endl;
                            }
                        }

                        // export drill
                        {
                            std::cout << "Export drill" << std::endl;
                            std::string command = "kicad-cli pcb export drill --excellon-separate-th";
                            if (job.manufacturer == Manufacturer::JLCPCB)
                                command += " --excellon-oval-format";
                            command += " --generate-map --map-format gerberx2 --output " + gerberDir.string() + ' ' + job.pcbPath.string();
                            int result = std::system(command.c_str());
                            if (result != 0) {
                                error = true;
                                std::cerr << "Error: Drill export, kicad-cli returned result " << result << std::endl;
                            }
                        }

                        // zip gerber
                        std::cout << "Zip gerber" << std::endl;
                        auto zipPath = outDir / (job.name + version + ".zip");

                        // create new zip
                        ZipArchive zip(zipPath.string());
                        if (zip.open(ZipArchive::New)) {
                            // add files
                            fs::directory_iterator end;
                            for (fs::directory_iterator it(gerberDir); it != end; ++it) {
                                if (it->is_regular_file()) {
                                    // read file
                                    fs::path path = it->path();

                                    // check last write time
                                    if (fs::last_write_time(path) < pcbTime) {
                                        std::cerr << "Error: File is not up-to-date: " << path.string() << std::endl;
                                        error = true;
                                    }

                                    if (!zip.addFile(path.filename().string(), path.string())) {
                                        std::cerr << "Error: Could add file to zip" << std::endl;
                                        error = true;
                                    }
                                }
                            }
                            zip.close();
                        } else {
                            std::cerr << "Error: Could not write zip file: " << zipPath.string() << std::endl;
                            error = true;
                        }
                    } else {
                        std::cerr << "Error: Gerber directory not found: " << gerberDir.string() << std::endl;
                        error = true;
                    }
                } else {
                    std::cerr << "Error: Section 'pcbplotparams' not found in .kicad_pcb" << std::endl;
                    error = true;
                }
            } else {
                std::cerr << "Error: Section 'setup' not found in .kicad_pcb" << std::endl;
                error = true;
            }
        }

        if (job.bom && manufacturer == Manufacturer::GENERIC) {
            // open generic BOM file
            fs::path bomPath = outDir / (job.name + version + ".csv");
            std::ofstream bom(bomPath);
            if (bom.is_open()) {
                std::map<BomKey, BomValue> bomMap;

                //bom << "Count,Type,Value,Voltage,Footprint,SMD Pads,THT Pads,Manufacturer,MPN,Description" << std::endl;
                bom << "Count,Reference,Value,Voltage,Footprint,SMD Pads,THT Pads,Manufacturer,MPN,Description" << std::endl;

                for (auto element1 : file.elements) {
                    auto container1 = dynamic_cast<kicad::Container *>(element1);
                    if (container1) {
                        // check if it is a footprint
                        if (container1->id == "footprint") {
                            auto footprint = container1;

                            // get footprint name
                            auto footprintName = footprint->getString(0);

                            // remove library from footprint name
                            auto pos = footprintName.find(':');
                            if (pos != std::string::npos)
                                footprintName.erase(0, pos + 1);

                            // get footprint properties
                            //std::string type; // e.g. "R" or "C"
                            std::string reference; // e.g. "R1"
                            std::string value; // e.g. 100k
                            int voltage = 0;
                            //int padCount = 0;
                            std::set<std::string> padNames; // to detect duplicates
                            std::string manufacturer;
                            std::string mpn;
                            std::string description;
                            bool doNotPopulate = false;
                            bool excludeFromBom = false;
                            bool throughHole = false;
                            for (auto element2 : container1->elements) {
                                auto property = dynamic_cast<kicad::Container *>(element2);
                                if (property) {
                                    if (property->id == "property") {
                                        auto propertyName = property->getString(0);
                                        auto propertyValue = property->getString(1);
                                        if (propertyName == "Reference") {
                                            // reference, e.g. "R1"
                                            reference = propertyValue;
                                        } else if (propertyName == "Value") {
                                            // value, e.g. "100k"
                                            value = propertyValue;
                                        } else if (propertyName == "Voltage") {
                                            // operating voltage
                                            voltage = lround(std::stod(propertyValue) * 1000.0);
                                        } else if (propertyName == "Manufacturer") {
                                            manufacturer = propertyValue;
                                        } else if (propertyName == "MPN") {
                                            // manufacturer part number
                                            mpn = propertyValue;
                                        } else if (propertyName == "Description") {
                                            description = propertyValue;
                                        }
                                    }
                                    if (property->id == "attr") {
                                        doNotPopulate = property->contains("dnp");
                                        excludeFromBom = property->contains("exclude_from_bom");
                                        throughHole = property->contains("through_hole");
                                    }
                                    if (property->id == "pad") {
                                        auto padName = property->getString(0);
                                        padNames.insert(padName);
                                        //++padCount;
                                    }
                                }
                            }

                            if (!excludeFromBom) {
                                auto &v = bomMap[{getType(reference), value, voltage, footprintName, manufacturer, mpn}];
                                v.references.push_back(reference);
                                int padCount = padNames.size();
                                v.padCount = std::max(v.padCount, padCount);
                                v.throughHole = throughHole;
                                v.description = description;
                            }
                        }
                    }
                }

                // write BOM
                for (auto &p : bomMap) {
                    // count
                    bom << p.second.references.size() << ",";

                    // type
                    //bom << p.first.type << ",";

                    // references
                    bom << '"';
                    for (auto &reference : p.second.references) {
                        if (reference != p.second.references.front())
                            bom << ',';
                        bom << reference;
                    }
                    bom << "\",";

                    // value, voltage, footprint
                    bom << "\"" << p.first.value << "\","
                        << (p.first.voltage * 0.001) << ","
                        << p.first.footprint << ",";

                    // pad count
                    if (p.second.throughHole)
                        bom << ',';
                    bom << p.second.padCount << ",";
                    if (!p.second.throughHole)
                        bom << ',';

                    // manufactuer, part number, description
                    bom << "\"" << p.first.manufacturer << "\","
                        << p.first.mpn << ","
                        "\"" << p.second.description << "\"" << std::endl;
                }
                bom.close();
            } else {
                std::cout << "Error: Could not create BOM file in " << outDir.string() << std::endl;
                error = true;
            }
        }

        if (job.bom && manufacturer == Manufacturer::JLCPCB) {
            // open BOM file for JLCPCB
            fs::path bomPath = outDir / (job.name + version + "-BOM.csv");
            std::ofstream bom(bomPath);

            // open CPL file
            fs::path cplPath = outDir / (job.name + version + "-CPL.csv");
            std::ofstream cpl(cplPath);

            if (bom.is_open() && cpl.is_open()) {
                // map from part protperties (e.g. footprint) to list of references (e.g. R1, R2, R3...)
                std::map<JlcBomKey, std::vector<std::string>> bomMap;

                // set of used references to detect duplicates
                std::set<std::string> usedReferences;

                bom << "Comment,Designator,Footprint,LCSC PN" << std::endl;
                cpl << "Designator,Mid X,Mid Y,Rotation,Layer" << std::endl;
                for (auto element1 : file.elements) {
                    auto container1 = dynamic_cast<kicad::Container *>(element1);
                    if (container1) {
                        // check if it is a footprint
                        if (container1->id == "footprint") {
                            auto footprint = container1;

                            // get footprint name
                            auto footprintName = footprint->getString(0);
                            //std::cout << "Footprint: " << footprintName << std::endl;

                            // remove library from footprint name
                            auto pos = footprintName.find(':');
                            if (pos != std::string::npos)
                                footprintName.erase(0, pos + 1);

                            // get layer
                            auto layer = footprint->findString("layer");

                            // get footprint properties
                            std::string x, y, rot;
                            std::string reference;
                            std::string value;
                            std::string lcscPn;
                            bool doNotPopulate = false;
                            bool excludeFromBom = false;
                            for (auto element2 : container1->elements) {
                                auto property = dynamic_cast<kicad::Container *>(element2);
                                if (property) {
                                    if (property->id == "at") {
                                        x = property->getString(0);
                                        y = property->getString(1);
                                        rot = property->getString(2, "0");
                                    }
                                    if (property->id == "property") {
                                        auto propertyName = property->getString(0);
                                        auto propertyValue = property->getString(1);
                                        if (propertyName == "Reference") {
                                            // reference (designator), e.g. "R1"
                                            reference = propertyValue;
                                        } else if (propertyName == "Value") {
                                            // value, e.g. "100k"
                                            value = propertyValue;
                                        } else if (propertyName == "LCSC PN") {
                                            // LCSC part number
                                            lcscPn = propertyValue;
                                        }
                                    }
                                    if (property->id == "attr") {
                                        doNotPopulate = property->contains("dnp");
                                        excludeFromBom = property->contains("exclude_from_bom");
                                    }
                                }
                            }

                            if (!doNotPopulate && !excludeFromBom) {
                                // check for duplicate reference
                                if (usedReferences.contains(reference)) {
                                    std::cout << "Error: Duplicate reference " << reference << std::endl;
                                    error = true;
                                }
                                usedReferences.insert(reference);

                                //std::cout << "add " << footprint << " reference " << reference << " value " << value << " at " << x << ' ' << y << ' ' << rot << std::endl;

                                bomMap[{getType(reference), value, footprintName, lcscPn}].push_back(reference);

                                std::string side = layer == "F.Cu" ? "top" : "bottom";

                                // write line to CPL file
                                cpl << reference << ',' << x << ",-" << y << ',' << rot << "," << side << std::endl;
                            } else {
                                //std::cout << "reject " << footprint << " reference " << reference << " value " << value << std::endl;
                            }
                        }
                    }
                }
                cpl.close();

                // print BOM to std::cout
                /*for (auto &p : bomMap) {
                    int count = p.second.size();
                    std::cout << count << "x " << p.first.value << " (" << p.first.footprintName << ')';
                    for (auto &reference : p.second) std::cout << " " << reference;
                    std::cout << std::endl;
                }*/

                // write BOM
                std::cout << "Write BOM" << std::endl;
                for (auto &p : bomMap) {
                    // comment (use value)
                    bom << p.first.value;

                    // quoted list of references
                    bom << ",\"";
                    bool first = true;
                    std::ranges::sort(p.second);
                    for (auto &reference : p.second) {
                        if (!first)
                            bom << ',';
                        first = false;
                        bom << reference;
                    }
                    bom << "\",";

                    // footprint and LCSC PN
                    bom <<  p.first.footprintName << ',' << p.first.lcscPn << std::endl;
                }
                bom.close();
            } else {
                std::cout << "Error: Could not create BOM/CPL file in " << outDir.string() << std::endl;
                error = true;
            }
        }

        if (job.drill) {
            // open drill file for OpenSCAD export
            fs::path drillPath = outDir / (job.name + ".scad");
            std::ofstream drillFile(drillPath);

            for (auto container1 : file) {
                // check if it is a footprint
                if (container1->id == "footprint") {
                    auto footprint = container1;

                    // get footprint name
                    auto footprintName = footprint->getString(0);
                    //std::cout << "Footprint: " << footprintName << std::endl;

                    // get position and rotation of footprint
                    double2 position = {0, 0};
                    double rotation = 0;
                    for (auto property : *footprint) {
                        if (property->id == "at") {
                            auto at = property;
                            position.x = at->getNumber(0);
                            position.y = at->getNumber(1);
                            rotation = at->getNumber(2);
                        }
                    }

                    // get drill holes
                    bool first = true;
                    for (auto property : *footprint) {
                        if (property->id == "pad") {
                            auto pad = property;

                            std::string type = pad->getTag(1);
                            if (type == "thru_hole" || type == "np_thru_hole") {
                                // get pad name
                                std::string padName = pad->getString(0);
                                //std::cout << "  Pad: " << padName << std::endl;

                                // get pad position
                                auto at = pad->find("at");
                                auto x = at->getNumber(0);
                                auto y = at->getNumber(1);

                                // get drill size
                                auto drill = pad->find("drill");
                                double w, h;
                                if (drill->elements.size() == 1) {
                                    w = h = drill->getNumber(0);
                                } else {
                                    w = drill->getNumber(1);
                                    h = drill->getNumber(2);
                                }

                                // transform to global coordinates
                                double r = rotation * pi / 180.0;
                                double s = sin(r);
                                double c = cos(r);
                                double gX = position.x + c * x + s * y;
                                double gY = position.y + c * y - s * x;

                                if (first) {
                                    first = false;
                                    drillFile << "// " << footprintName << std::endl;
                                }
                                drillFile << "drill(" << gX << ", " << gY << ", " << w << ", " << h << ", " << rotation << ");";
                                if (!padName.empty())
                                    drillFile << " // " << padName;
                                drillFile << std::endl;
                            }
                        }
                    }
                    if (!first)
                        drillFile << std::endl;
                }
            }
        }
    }

    std::cout << std::endl;
    if (error)
        std::cout << "!!! There were errors !!!" << std::endl;
    else
        std::cout << "*** OK ***" << std::endl;
    return 0;
}
