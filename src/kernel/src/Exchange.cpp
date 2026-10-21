// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Exchange.h>

#include <lcad/core/Path.h>
#include <lcad/core/Version.h>
#include <lcad/kernel/Error.h>
#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <APIHeaderSection_MakeHeader.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Reader.hxx>
#include <Interface_InterfaceModel.hxx>
#include <Interface_Static.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Solid.hxx>
#include <StepBasic_Product.hxx>
#include <StepBasic_ProductDefinition.hxx>
#include <StepBasic_ProductDefinitionFormation.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TransferBRep.hxx>
#include <Transfer_TransientProcess.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>

#include <zip.h>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numbers>
#include <random>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace lcad::kernel {

namespace {

// Faces whose edges are closer than this, in millimeters, are sewn together on import.
constexpr double sewingTolerance = 1e-3;

// OCCT's translators take their settings from one global table, which a translator fills in
// when it is first set up: a setting made before that is lost.
std::mutex& translatorSettings() {
    static std::mutex mutex;
    STEPControl_Controller::Init();
    IGESControl_Controller::Init();
    return mutex;
}

std::string fileName(const std::filesystem::path& path) {
    return core::toUtf8(path.filename());
}

void requireReadable(const std::filesystem::path& path, const std::string& operation) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        throw Error(operation, "the file does not exist");
    }
    if (std::filesystem::is_directory(path, ec)) {
        throw Error(operation, "it is a folder, not a file");
    }
    std::ifstream probe(path, std::ios::binary);
    if (!probe) {
        throw Error(operation, "the file cannot be opened for reading");
    }
}

// Writes a file under a temporary name next to `path`, then puts it in place, so that a
// failure leaves any file that was there as it was.
void writeReplacing(const std::filesystem::path& path, const std::string& operation,
                    const std::function<void(const std::filesystem::path&)>& write) {
    std::error_code ec;
    const std::filesystem::path target = std::filesystem::absolute(path, ec);
    const std::filesystem::path directory = target.parent_path();
    if (!std::filesystem::is_directory(directory, ec)) {
        throw Error(operation, "the folder " + core::toUtf8(directory) + " does not exist");
    }
    if (std::filesystem::is_directory(target, ec)) {
        throw Error(operation, "it is a folder; choose a file name inside it");
    }
    std::random_device random;
    const std::filesystem::path temporary =
        directory / ("." + core::toUtf8(target.filename()) + "." + std::to_string(random()) + ".tmp");
    try {
        write(temporary);
        std::filesystem::rename(temporary, target, ec);
        if (ec) {
            throw Error(operation, ec.message());
        }
    } catch (...) {
        std::filesystem::remove(temporary, ec);
        throw;
    }
}

// How many edges of a set of faces border only one face of it.
int openEdgesOf(const TopoDS_Shape& faces) {
    ShapeAnalysis_FreeBounds bounds(faces, sewingTolerance, Standard_False, Standard_False);
    int count = 0;
    for (const TopoDS_Compound& wires : {bounds.GetClosedWires(), bounds.GetOpenWires()}) {
        for (TopExp_Explorer edge(wires, TopAbs_EDGE); edge.More(); edge.Next()) {
            ++count;
        }
    }
    return count;
}

int countOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
    int count = 0;
    for (TopExp_Explorer it(shape, type); it.More(); it.Next()) {
        ++count;
    }
    return count;
}

std::string plural(int count, const std::string& one, const std::string& many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// Turns what a translator read into bodies: each solid as it is, and the faces that belong to
// no solid sewn together, those that close up made solids. `nameOf` gives the name the file
// has for a solid, or an empty string.
ImportResult bodiesOf(const TopoDS_Shape& all, const std::string& fallback,
                      const std::function<std::string(const TopoDS_Shape&)>& nameOf) {
    ImportResult result;
    std::map<std::string, int> used;
    const auto add = [&](const TopoDS_Shape& shape, std::string name) {
        if (name.empty()) {
            name = fallback;
        }
        const int seen = used[name]++;
        if (seen > 0) {
            name += " " + std::to_string(seen + 1);
        }
        if (!BRepCheck_Analyzer(shape).IsValid()) {
            result.notes.push_back(name +
                                   " is not valid geometry as it was read. Check Geometry says "
                                   "what is wrong with it, and Repair may put it right.");
        }
        result.bodies.push_back({name, occt::wrap(shape)});
    };

    for (TopExp_Explorer solid(all, TopAbs_SOLID); solid.More(); solid.Next()) {
        add(solid.Current(), nameOf(solid.Current()));
    }

    // The faces that are not part of a solid.
    BRepBuilderAPI_Sewing sewing(sewingTolerance);
    int loose = 0;
    for (TopExp_Explorer face(all, TopAbs_FACE, TopAbs_SOLID); face.More(); face.Next()) {
        sewing.Add(face.Current());
        ++loose;
    }
    if (loose == 0) {
        return result;
    }
    sewing.Perform();
    const TopoDS_Shape sewn = sewing.SewedShape();
    int closed = 0;
    for (TopExp_Explorer it(sewn, TopAbs_SHELL); it.More(); it.Next()) {
        const TopoDS_Shell shell = TopoDS::Shell(it.Current());
        const int open = openEdgesOf(shell);
        if (open == 0) {
            BRepBuilderAPI_MakeSolid maker(shell);
            if (maker.IsDone()) {
                // Faces sewn together may face inward; a solid has them face out.
                ShapeFix_Solid fix(maker.Solid());
                fix.Perform();
                add(fix.Solid(), "");
                ++closed;
                continue;
            }
        }
        add(shell, "");
        result.notes.push_back(
            result.bodies.back().name + " is " +
            plural(countOf(shell, TopAbs_FACE), "face", "faces") +
            " that do not close up into a solid: " + plural(open, "edge borders", "edges border") +
            " only one face. It is kept as surfaces.");
    }
    // Faces sewing could not join to any other.
    BRep_Builder builder;
    TopoDS_Compound single;
    builder.MakeCompound(single);
    int singles = 0;
    for (TopExp_Explorer face(sewn, TopAbs_FACE, TopAbs_SHELL); face.More(); face.Next()) {
        builder.Add(single, face.Current());
        ++singles;
    }
    if (singles > 0) {
        add(single, "");
        result.notes.push_back(result.bodies.back().name + " is " +
                               plural(singles, "face", "faces") +
                               " that meet no other face. It is kept as surfaces.");
    }
    if (closed > 0) {
        result.notes.push_back("Loose faces in the file were sewn into " +
                               plural(closed, "closed solid.", "closed solids."));
    }
    return result;
}

std::string text(const Handle(TCollection_HAsciiString)& string) {
    return string.IsNull() ? std::string() : std::string(string->ToCString());
}

// The names the parts of a STEP file have, by the solids they are made of. A solid that is in
// several parts, such as a part and the assembly it is in, takes the name of the smallest.
std::unordered_map<const TopoDS_TShape*, std::string> partNames(STEPControl_Reader& reader) {
    std::unordered_map<const TopoDS_TShape*, std::pair<std::string, int>> best;
    const Handle(XSControl_WorkSession) session = reader.WS();
    const Handle(Interface_InterfaceModel) model = session->Model();
    const Handle(Transfer_TransientProcess) process = session->TransferReader()->TransientProcess();
    for (Standard_Integer i = 1; i <= model->NbEntities(); ++i) {
        const Handle(StepBasic_ProductDefinition) definition =
            Handle(StepBasic_ProductDefinition)::DownCast(model->Value(i));
        if (definition.IsNull() || definition->Formation().IsNull() ||
            definition->Formation()->OfProduct().IsNull()) {
            continue;
        }
        std::string name = text(definition->Formation()->OfProduct()->Name());
        // What OCCT writes when it is given no name.
        if (name.empty() || name.rfind("Open CASCADE STEP translator", 0) == 0) {
            continue;
        }
        const TopoDS_Shape shape = TransferBRep::ShapeResult(process, definition);
        if (shape.IsNull()) {
            continue;
        }
        const int solids = countOf(shape, TopAbs_SOLID);
        for (TopExp_Explorer solid(shape, TopAbs_SOLID); solid.More(); solid.Next()) {
            const TopoDS_TShape* key = solid.Current().TShape().get();
            const auto found = best.find(key);
            if (found == best.end() || solids < found->second.second) {
                best[key] = {name, solids};
            }
        }
    }
    std::unordered_map<const TopoDS_TShape*, std::string> names;
    for (const auto& [key, entry] : best) {
        names[key] = entry.first;
    }
    return names;
}

// Numbers as text that reads back to the same value, whatever the user's language.
void appendNumber(std::string& out, double value) {
    char buffer[32];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, error == std::errc() ? end : buffer);
}

std::string xmlEscaped(const std::string& value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\n':
        case '\r':
        case '\t':
            out += ' ';
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string oneLine(const std::string& value) {
    std::string out = value;
    for (char& c : out) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return out;
}

void requireBodies(const std::vector<NamedBody>& bodies, const std::string& operation) {
    if (bodies.empty()) {
        throw Error(operation, "there is nothing to export");
    }
    for (const NamedBody& body : bodies) {
        if (body.shape.isNull()) {
            throw std::invalid_argument(operation + ": " + body.name + " has no shape");
        }
    }
}

void requireSettings(const MeshSettings& settings) {
    if (!std::isfinite(settings.deviation) || settings.deviation <= 0.0) {
        throw std::invalid_argument("triangulate: the deviation must be greater than zero");
    }
    if (!std::isfinite(settings.angle) || settings.angle <= 0.0 || settings.angle > 90.0) {
        throw std::invalid_argument("triangulate: the angle must be more than 0 and at most 90");
    }
}

// Joins corners that are within a tiny distance of each other into one vertex.
class VertexMerger {
public:
    explicit VertexMerger(Mesh& mesh) : m_mesh(mesh) {}

    std::uint32_t add(const gp_Pnt& p) {
        const auto cell = [](double v) {
            return static_cast<long long>(std::floor(v / tolerance));
        };
        const long long cx = cell(p.X());
        const long long cy = cell(p.Y());
        const long long cz = cell(p.Z());
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto found = m_cells.find({cx + dx, cy + dy, cz + dz});
                    if (found == m_cells.end()) {
                        continue;
                    }
                    for (const std::uint32_t index : found->second) {
                        const Point& q = m_mesh.vertices[index];
                        if (std::abs(q.x - p.X()) <= tolerance &&
                            std::abs(q.y - p.Y()) <= tolerance &&
                            std::abs(q.z - p.Z()) <= tolerance) {
                            return index;
                        }
                    }
                }
            }
        }
        const auto index = static_cast<std::uint32_t>(m_mesh.vertices.size());
        m_mesh.vertices.push_back({p.X(), p.Y(), p.Z()});
        m_cells[{cx, cy, cz}].push_back(index);
        return index;
    }

private:
    static constexpr double tolerance = 1e-6;
    Mesh& m_mesh;
    std::map<std::tuple<long long, long long, long long>, std::vector<std::uint32_t>> m_cells;
};

std::array<double, 3> normalOf(const Mesh& mesh, const std::array<std::uint32_t, 3>& triangle) {
    const Point& a = mesh.vertices[triangle[0]];
    const Point& b = mesh.vertices[triangle[1]];
    const Point& c = mesh.vertices[triangle[2]];
    const double ux = b.x - a.x;
    const double uy = b.y - a.y;
    const double uz = b.z - a.z;
    const double vx = c.x - a.x;
    const double vy = c.y - a.y;
    const double vz = c.z - a.z;
    double nx = uy * vz - uz * vy;
    double ny = uz * vx - ux * vz;
    double nz = ux * vy - uy * vx;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (length > 0.0) {
        nx /= length;
        ny /= length;
        nz /= length;
    }
    return {nx, ny, nz};
}

std::vector<Mesh> meshesOf(const std::vector<NamedBody>& bodies, const MeshSettings& settings) {
    std::vector<Mesh> meshes;
    meshes.reserve(bodies.size());
    for (const NamedBody& body : bodies) {
        meshes.push_back(triangulate(body.shape, settings));
    }
    return meshes;
}

void appendLittleEndian(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out += static_cast<char>((value >> (8 * i)) & 0xFFU);
    }
}

void appendFloat(std::string& out, double value) {
    appendLittleEndian(out, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes,
                const std::string& operation) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw Error(operation, "the file could not be written");
    }
}

} // namespace

ImportResult importStep(const std::filesystem::path& path) {
    const std::string operation = "read " + fileName(path);
    requireReadable(path, operation);
    return occt::guarded(operation.c_str(), [&] {
        const std::lock_guard lock(translatorSettings());
        // Lengths in millimeters, whatever unit the file is in.
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        STEPControl_Reader reader;
        if (reader.ReadFile(core::toUtf8(path).c_str()) != IFSelect_RetDone) {
            throw Error(operation, "it is not a STEP file, or it is damaged");
        }
        reader.TransferRoots();
        const TopoDS_Shape all = reader.OneShape();
        if (all.IsNull() || countOf(all, TopAbs_FACE) == 0) {
            throw Error(operation, "it has no solids or surfaces in it");
        }
        const auto names = partNames(reader);
        return bodiesOf(all, core::toUtf8(path.stem()), [&names](const TopoDS_Shape& solid) {
            const auto found = names.find(solid.TShape().get());
            return found == names.end() ? std::string() : found->second;
        });
    });
}

ImportResult importIges(const std::filesystem::path& path) {
    const std::string operation = "read " + fileName(path);
    requireReadable(path, operation);
    return occt::guarded(operation.c_str(), [&] {
        const std::lock_guard lock(translatorSettings());
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        IGESControl_Reader reader;
        if (reader.ReadFile(core::toUtf8(path).c_str()) != IFSelect_RetDone || reader.NbRootsForTransfer() == 0) {
            throw Error(operation, "it is not an IGES file, or it has no geometry in it");
        }
        reader.TransferRoots();
        const TopoDS_Shape all = reader.OneShape();
        if (all.IsNull() || countOf(all, TopAbs_FACE) == 0) {
            throw Error(operation, "it has no solids or surfaces in it");
        }
        return bodiesOf(all, core::toUtf8(path.stem()),
                        [](const TopoDS_Shape&) { return std::string(); });
    });
}

void exportStep(const std::vector<NamedBody>& bodies, const std::filesystem::path& path) {
    const std::string operation = "write " + fileName(path);
    requireBodies(bodies, operation);
    occt::guarded(operation.c_str(), [&] {
        const std::lock_guard lock(translatorSettings());
        Interface_Static::SetCVal("xstep.cascade.unit", "MM");
        Interface_Static::SetCVal("write.step.unit", "MM");
        STEPControl_Writer writer;
        for (const NamedBody& body : bodies) {
            const Standard_Integer before = writer.Model()->NbEntities();
            if (writer.Transfer(occt::unwrap(body.shape), STEPControl_AsIs) != IFSelect_RetDone) {
                throw Error(operation, body.name + " could not be put into STEP");
            }
            // Each body is a part of its own. OCCT names it after a setting, with a number
            // added; it is given the body's name instead.
            const Handle(TCollection_HAsciiString) name =
                new TCollection_HAsciiString(body.name.c_str());
            for (Standard_Integer i = before + 1; i <= writer.Model()->NbEntities(); ++i) {
                const Handle(StepBasic_Product) product =
                    Handle(StepBasic_Product)::DownCast(writer.Model()->Value(i));
                if (!product.IsNull()) {
                    product->SetId(name);
                    product->SetName(name);
                }
            }
        }
        // The header says what the file is and what wrote it; OCCT's own fills it with
        // placeholders.
        APIHeaderSection_MakeHeader header(writer.Model());
        header.SetName(new TCollection_HAsciiString(core::toUtf8(path.filename()).c_str()));
        header.SetDescriptionValue(1, new TCollection_HAsciiString("LCAD model"));
        header.SetAuthorValue(1, new TCollection_HAsciiString(""));
        header.SetOrganizationValue(1, new TCollection_HAsciiString(""));
        header.SetOriginatingSystem(
            new TCollection_HAsciiString(("LCAD " + core::version()).c_str()));
        header.Apply(writer.Model());
        writeReplacing(path, operation, [&](const std::filesystem::path& temporary) {
            if (writer.Write(core::toUtf8(temporary).c_str()) != IFSelect_RetDone) {
                throw Error(operation, "the file could not be written");
            }
        });
    });
}

Mesh triangulate(const Shape& shape, const MeshSettings& settings) {
    if (shape.isNull()) {
        throw std::invalid_argument("triangulate: the shape is null");
    }
    requireSettings(settings);
    return occt::guarded("make a mesh", [&] {
        // A copy, so that the triangles made for the file are not left on the shape the 3D
        // view draws.
        const TopoDS_Shape copy =
            BRepBuilderAPI_Copy(occt::unwrap(shape), Standard_True, Standard_False).Shape();
        const BRepMesh_IncrementalMesh mesher(copy, settings.deviation, Standard_False,
                                              settings.angle * std::numbers::pi / 180.0,
                                              Standard_True);
        Mesh mesh;
        VertexMerger merger(mesh);
        int faces = 0;
        for (TopExp_Explorer it(copy, TopAbs_FACE); it.More(); it.Next()) {
            ++faces;
            const TopoDS_Face& face = TopoDS::Face(it.Current());
            TopLoc_Location location;
            const Handle(Poly_Triangulation) triangulation =
                BRep_Tool::Triangulation(face, location);
            if (triangulation.IsNull()) {
                throw Error("make a mesh", "a face could not be cut into triangles");
            }
            const gp_Trsf placement = location.Transformation();
            std::vector<std::uint32_t> corners(static_cast<std::size_t>(triangulation->NbNodes()) +
                                               1);
            for (Standard_Integer i = 1; i <= triangulation->NbNodes(); ++i) {
                corners[static_cast<std::size_t>(i)] =
                    merger.add(triangulation->Node(i).Transformed(placement));
            }
            // The triangles go round the way the surface faces; a face that is turned the
            // other way in the shape has them turned too.
            const bool reversed = face.Orientation() == TopAbs_REVERSED;
            for (Standard_Integer i = 1; i <= triangulation->NbTriangles(); ++i) {
                Standard_Integer a = 0;
                Standard_Integer b = 0;
                Standard_Integer c = 0;
                triangulation->Triangle(i).Get(a, b, c);
                if (reversed) {
                    std::swap(b, c);
                }
                const std::array<std::uint32_t, 3> triangle{corners[static_cast<std::size_t>(a)],
                                                            corners[static_cast<std::size_t>(b)],
                                                            corners[static_cast<std::size_t>(c)]};
                // Corners merged into one leave a triangle with no area.
                if (triangle[0] != triangle[1] && triangle[1] != triangle[2] &&
                    triangle[0] != triangle[2]) {
                    mesh.triangles.push_back(triangle);
                }
            }
        }
        if (faces == 0 || mesh.triangles.empty()) {
            throw Error("make a mesh", "the shape has no faces to cut into triangles");
        }
        return mesh;
    });
}

void exportStl(const std::vector<NamedBody>& bodies, const std::filesystem::path& path,
               const MeshSettings& settings, StlEncoding encoding) {
    const std::string operation = "write " + fileName(path);
    requireBodies(bodies, operation);
    const std::vector<Mesh> meshes = meshesOf(bodies, settings);
    std::string out;
    if (encoding == StlEncoding::Binary) {
        std::string header = "Written by LCAD. Units: millimeters.";
        header.resize(80, ' ');
        out += header;
        std::uint32_t count = 0;
        for (const Mesh& mesh : meshes) {
            count += static_cast<std::uint32_t>(mesh.triangles.size());
        }
        appendLittleEndian(out, count);
        for (const Mesh& mesh : meshes) {
            for (const auto& triangle : mesh.triangles) {
                for (const double n : normalOf(mesh, triangle)) {
                    appendFloat(out, n);
                }
                for (const std::uint32_t corner : triangle) {
                    const Point& p = mesh.vertices[corner];
                    appendFloat(out, p.x);
                    appendFloat(out, p.y);
                    appendFloat(out, p.z);
                }
                out += '\0';
                out += '\0';
            }
        }
    } else {
        // One solid for all of them: not every program reads several from one file.
        const std::string name = bodies.size() == 1 ? oneLine(bodies.front().name) : "LCAD";
        out += "solid " + name + "\n";
        for (const Mesh& mesh : meshes) {
            for (const auto& triangle : mesh.triangles) {
                const auto n = normalOf(mesh, triangle);
                out += "  facet normal ";
                for (int i = 0; i < 3; ++i) {
                    appendNumber(out, n[static_cast<std::size_t>(i)]);
                    out += i < 2 ? ' ' : '\n';
                }
                out += "    outer loop\n";
                for (const std::uint32_t corner : triangle) {
                    const Point& p = mesh.vertices[corner];
                    out += "      vertex ";
                    appendNumber(out, p.x);
                    out += ' ';
                    appendNumber(out, p.y);
                    out += ' ';
                    appendNumber(out, p.z);
                    out += '\n';
                }
                out += "    endloop\n  endfacet\n";
            }
        }
        out += "endsolid " + name + "\n";
    }
    writeReplacing(path, operation, [&](const std::filesystem::path& temporary) {
        writeBytes(temporary, out, operation);
    });
}

void exportObj(const std::vector<NamedBody>& bodies, const std::filesystem::path& path,
               const MeshSettings& settings) {
    const std::string operation = "write " + fileName(path);
    requireBodies(bodies, operation);
    const std::vector<Mesh> meshes = meshesOf(bodies, settings);
    std::string out = "# Written by LCAD. Units: millimeters.\n";
    std::size_t offset = 1;
    for (std::size_t b = 0; b < bodies.size(); ++b) {
        const Mesh& mesh = meshes[b];
        out += "o " + oneLine(bodies[b].name) + "\n";
        for (const Point& p : mesh.vertices) {
            out += "v ";
            appendNumber(out, p.x);
            out += ' ';
            appendNumber(out, p.y);
            out += ' ';
            appendNumber(out, p.z);
            out += '\n';
        }
        for (const auto& triangle : mesh.triangles) {
            out += "f";
            for (const std::uint32_t corner : triangle) {
                out += ' ' + std::to_string(offset + corner);
            }
            out += '\n';
        }
        offset += mesh.vertices.size();
    }
    writeReplacing(path, operation, [&](const std::filesystem::path& temporary) {
        writeBytes(temporary, out, operation);
    });
}

void export3mf(const std::vector<NamedBody>& bodies, const std::filesystem::path& path,
               const MeshSettings& settings) {
    const std::string operation = "write " + fileName(path);
    requireBodies(bodies, operation);
    const std::vector<Mesh> meshes = meshesOf(bodies, settings);

    std::string model = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<model unit=\"millimeter\" xml:lang=\"en-US\" "
                        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
                        " <metadata name=\"Application\">LCAD</metadata>\n"
                        " <resources>\n";
    for (std::size_t b = 0; b < bodies.size(); ++b) {
        const Mesh& mesh = meshes[b];
        model += "  <object id=\"" + std::to_string(b + 1) + "\" type=\"model\" name=\"" +
                 xmlEscaped(bodies[b].name) + "\">\n   <mesh>\n    <vertices>\n";
        for (const Point& p : mesh.vertices) {
            model += "     <vertex x=\"";
            appendNumber(model, p.x);
            model += "\" y=\"";
            appendNumber(model, p.y);
            model += "\" z=\"";
            appendNumber(model, p.z);
            model += "\"/>\n";
        }
        model += "    </vertices>\n    <triangles>\n";
        for (const auto& triangle : mesh.triangles) {
            model += "     <triangle v1=\"" + std::to_string(triangle[0]) + "\" v2=\"" +
                     std::to_string(triangle[1]) + "\" v3=\"" + std::to_string(triangle[2]) +
                     "\"/>\n";
        }
        model += "    </triangles>\n   </mesh>\n  </object>\n";
    }
    model += " </resources>\n <build>\n";
    for (std::size_t b = 0; b < bodies.size(); ++b) {
        model += "  <item objectid=\"" + std::to_string(b + 1) + "\"/>\n";
    }
    model += " </build>\n</model>\n";

    const std::vector<std::pair<std::string, std::string>> entries = {
        {"[Content_Types].xml",
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
         "<Default Extension=\"rels\" "
         "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
         "<Default Extension=\"model\" "
         "ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>"
         "</Types>\n"},
        {"_rels/.rels",
         "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
         "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>"
         "</Relationships>\n"},
        {"3D/3dmodel.model", model},
    };
    writeReplacing(path, operation, [&](const std::filesystem::path& temporary) {
        int code = 0;
        zip_t* archive = zip_open(core::toUtf8(temporary).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &code);
        if (archive == nullptr) {
            throw Error(operation, "the file could not be created");
        }
        for (const auto& [name, bytes] : entries) {
            // The archive refers to `bytes` without copying; they stay alive until zip_close().
            zip_source_t* source = zip_source_buffer(archive, bytes.data(), bytes.size(), 0);
            if (source == nullptr ||
                zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
                const std::string reason = zip_strerror(archive);
                if (source != nullptr) {
                    zip_source_free(source);
                }
                zip_discard(archive);
                throw Error(operation, reason);
            }
        }
        if (zip_close(archive) != 0) {
            const std::string reason = zip_strerror(archive);
            zip_discard(archive);
            throw Error(operation, reason);
        }
    });
}

} // namespace lcad::kernel
