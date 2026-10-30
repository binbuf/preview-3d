// T08 fixture generator. Test-only: it links OCCT's readers/writers to create
// the small IGES and BREP fixtures committed under tests/fixtures/cad-spike.
// No product target ever links this file or an OCCT writer.
//
// Build with the constrained compatibility-host-step OCCT closure (TKDEIGES is
// now part of it) and run from the repository root:
//
//   GenerateCadFixtures.exe tests/fixtures/cad-spike
//
// The committed files are durable artifacts; OCCT embeds a creation timestamp,
// so regenerating does not reproduce the bytes exactly.

#define NOMINMAX
#include <windows.h>

#pragma warning(push, 0)
#include <BinXCAFDrivers.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESCAFControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Application.hxx>
#include <TDocStd_Document.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#pragma warning(pop)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string Narrow(const std::wstring& wide)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1)
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), length, nullptr, nullptr);
    return result;
}

Handle(TDocStd_Document) NewDocument()
{
    Handle(TDocStd_Document) document;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", document);
    return document;
}

bool WriteIges(const Handle(TDocStd_Document)& document, const std::filesystem::path& path)
{
    IGESCAFControl_Writer writer;
    writer.SetColorMode(Standard_True);
    writer.SetNameMode(Standard_True);
    if (!writer.Transfer(document)) return false;
    return writer.Write(Narrow(path.wstring()).c_str()) == IFSelect_RetDone;
}

TopoDS_Shape PartWithHole()
{
    const TopoDS_Shape block = BRepPrimAPI_MakeBox(30.0, 30.0, 10.0).Shape();
    const TopoDS_Shape tool = BRepPrimAPI_MakeCylinder(5.0, 40.0).Shape();
    return BRepAlgoAPI_Cut(block, tool).Shape();
}

bool WritePartIges(const std::filesystem::path& path)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());
    const TDF_Label label = shapes->AddShape(PartWithHole(), Standard_False);
    colors->SetColor(label, Quantity_Color(0.2, 0.7, 0.3, Quantity_TOC_RGB), XCAFDoc_ColorGen);
    return WriteIges(document, path);
}

bool WriteAssemblyIges(const std::filesystem::path& path)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());

    const TDF_Label boxLabel = shapes->AddShape(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(),
                                                Standard_False);
    colors->SetColor(boxLabel, Quantity_Color(0.9, 0.1, 0.1, Quantity_TOC_RGB), XCAFDoc_ColorGen);
    const TDF_Label cylinderLabel = shapes->AddShape(BRepPrimAPI_MakeCylinder(3.0, 12.0).Shape(),
                                                     Standard_False);
    colors->SetColor(cylinderLabel, Quantity_Color(0.1, 0.4, 0.9, Quantity_TOC_RGB),
                     XCAFDoc_ColorGen);

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const TDF_Label assembly = shapes->AddShape(compound, Standard_True);
    gp_Trsf first;
    shapes->AddComponent(assembly, boxLabel, TopLoc_Location(first));
    gp_Trsf second;
    second.SetTranslation(gp_Vec(40.0, 0.0, 0.0));
    shapes->AddComponent(assembly, boxLabel, TopLoc_Location(second));
    gp_Trsf third;
    third.SetTranslation(gp_Vec(5.0, 5.0, 15.0));
    shapes->AddComponent(assembly, cylinderLabel, TopLoc_Location(third));
    shapes->UpdateAssemblies();
    return WriteIges(document, path);
}

bool WriteAssemblyXbf(const std::filesystem::path& path)
{
    const Handle(TDocStd_Application) app = XCAFApp_Application::GetApplication();
    BinXCAFDrivers::DefineFormat(app);
    Handle(TDocStd_Document) document;
    app->NewDocument("BinXCAF", document);
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());
    XCAFDoc_DocumentTool::SetLengthUnit(document, 0.001);

    const TDF_Label boxLabel = shapes->AddShape(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(),
                                                Standard_False);
    colors->SetColor(boxLabel, Quantity_Color(0.9, 0.1, 0.1, Quantity_TOC_RGB), XCAFDoc_ColorGen);
    const TDF_Label cylinderLabel = shapes->AddShape(BRepPrimAPI_MakeCylinder(3.0, 12.0).Shape(),
                                                     Standard_False);
    colors->SetColor(cylinderLabel, Quantity_Color(0.1, 0.4, 0.9, Quantity_TOC_RGB),
                     XCAFDoc_ColorGen);

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const TDF_Label assembly = shapes->AddShape(compound, Standard_True);
    shapes->AddComponent(assembly, boxLabel, TopLoc_Location(gp_Trsf()));
    gp_Trsf second;
    second.SetTranslation(gp_Vec(40.0, 0.0, 0.0));
    shapes->AddComponent(assembly, boxLabel, TopLoc_Location(second));
    gp_Trsf third;
    third.SetTranslation(gp_Vec(5.0, 5.0, 15.0));
    shapes->AddComponent(assembly, cylinderLabel, TopLoc_Location(third));
    shapes->UpdateAssemblies();

    const TCollection_ExtendedString wide(path.wstring().c_str());
    return app->SaveAs(document, wide) == PCDM_SS_OK;
}

bool WriteTruncatedXbf(const std::filesystem::path& directory)
{
    const std::filesystem::path source = directory / "assembly.xbf";
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 64) return false;
    bytes.resize(bytes.size() / 3);
    std::ofstream output(directory / "truncated.xbf", std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool WriteMalformedXbf(const std::filesystem::path& directory)
{
    const std::filesystem::path source = directory / "assembly.xbf";
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 15) return false;
    // Keep the ASCII magic, replace the 64-bit format version with an
    // unsupported value so product-owned admission must reject it.
    for (std::size_t index = 7; index < 15; ++index) bytes[index] = '\x7f';
    std::ofstream output(directory / "malformed.xbf", std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool WritePartBrep(const std::filesystem::path& path)
{
    return BRepTools::Write(PartWithHole(), Narrow(path.wstring()).c_str());
}

bool WriteCompoundBrep(const std::filesystem::path& path)
{
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
    TopoDS_Shape translated = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(20.0, 0.0, 0.0));
    translated.Move(TopLoc_Location(shift));
    builder.Add(compound, translated);
    return BRepTools::Write(compound, Narrow(path.wstring()).c_str());
}

bool WriteTruncatedIges(const std::filesystem::path& directory)
{
    const std::filesystem::path source = directory / "part.igs";
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 400) return false;
    bytes.resize(bytes.size() / 3);
    std::ofstream output(directory / "truncated.igs", std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool WriteMalformedBrep(const std::filesystem::path& directory)
{
    // Real header, then a TShapes table that promises more records than exist.
    static const char kText[] =
        "\nCASCADE Topology V1, (c) Matra-Datavision\n"
        "Locations 0\n"
        "Curve2ds 0\nCurves 0\nPolygon3D 0\nPolygonOnTriangulations 0\n"
        "Surfaces 0\nTriangulations 0\n\n"
        "TShapes 7\n";
    std::ofstream output(directory / "malformed.brep", std::ios::binary | std::ios::trunc);
    output.write(kText, sizeof(kText) - 1);
    return output.good();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : L"tests/fixtures/cad-spike";
    std::error_code error;
    std::filesystem::create_directories(output, error);
    XCAFApp_Application::GetApplication();

    struct Case { const wchar_t* name; bool (*write)(const std::filesystem::path&); };
    const Case cases[] = {
        {L"part.igs", WritePartIges},
        {L"assembly.igs", WriteAssemblyIges},
        {L"assembly.xbf", WriteAssemblyXbf},
        {L"part.brep", WritePartBrep},
        {L"compound.brep", WriteCompoundBrep},
    };
    for (const auto& item : cases) {
        const std::filesystem::path path = output / item.name;
        if (!item.write(path)) {
            std::fprintf(stderr, "failed to write %ls\n", path.c_str());
            return 2;
        }
        std::printf("wrote %ls\n", path.c_str());
    }
    if (!WriteTruncatedIges(output) || !WriteMalformedBrep(output)
        || !WriteTruncatedXbf(output) || !WriteMalformedXbf(output)) {
        std::fprintf(stderr, "failed to write negative fixtures\n");
        return 3;
    }
    std::printf("wrote negative fixtures\n");
    return 0;
}
