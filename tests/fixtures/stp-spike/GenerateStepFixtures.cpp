#define NOMINMAX
#include <windows.h>

#pragma warning(push, 0)
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <Interface_Static.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#pragma warning(pop)

#include <cstdio>
#include <filesystem>
#include <string>

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

bool WriteDocument(const Handle(TDocStd_Document)& document, const std::filesystem::path& path,
                   const char* schema)
{
    Interface_Static::SetCVal("write.step.schema", schema);
    STEPCAFControl_Writer writer;
    writer.SetColorMode(Standard_True);
    writer.SetNameMode(Standard_True);
    writer.SetLayerMode(Standard_True);
    if (!writer.Transfer(document)) return false;
    const std::string narrow = Narrow(path.wstring());
    return writer.Write(narrow.c_str()) == IFSelect_RetDone;
}

// AP214 assembly: one shared box definition used twice, one cylinder, face
// colors, a nested transform, and millimetre units.
bool WriteAssembly(const std::filesystem::path& path)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());

    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    const TDF_Label boxLabel = shapes->AddShape(box, Standard_False);
    colors->SetColor(boxLabel, Quantity_Color(0.9, 0.1, 0.1, Quantity_TOC_RGB), XCAFDoc_ColorGen);

    const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(3.0, 12.0).Shape();
    const TDF_Label cylinderLabel = shapes->AddShape(cylinder, Standard_False);
    colors->SetColor(cylinderLabel, Quantity_ColorRGBA(0.1f, 0.4f, 0.9f, 0.35f),
                     XCAFDoc_ColorGen);

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, box);
    builder.Add(compound, cylinder);
    const TDF_Label assembly = shapes->AddShape(compound, Standard_True);

    gp_Trsf first;
    first.SetTranslation(gp_Vec(0.0, 0.0, 0.0));
    shapes->AddComponent(assembly, boxLabel, TopLoc_Location(first));

    gp_Trsf second;
    second.SetTranslation(gp_Vec(40.0, 0.0, 0.0));
    shapes->AddComponent(assembly, boxLabel, TopLoc_Location(second));

    gp_Trsf third;
    third.SetTranslation(gp_Vec(5.0, 5.0, 15.0));
    shapes->AddComponent(assembly, cylinderLabel, TopLoc_Location(third));

    shapes->UpdateAssemblies();
    return WriteDocument(document, path, "AP214IS");
}

// AP203 single analytic part with a through-hole (trimmed B-rep).
bool WritePart(const std::filesystem::path& path, const char* schema,
               double unitMeters = 0.001)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());

    const TopoDS_Shape block = BRepPrimAPI_MakeBox(30.0, 30.0, 10.0).Shape();
    const TopoDS_Shape tool = BRepPrimAPI_MakeCylinder(5.0, 40.0).Shape();
    const TopoDS_Shape part = BRepAlgoAPI_Cut(block, tool).Shape();
    const TDF_Label label = shapes->AddShape(part, Standard_False);
    colors->SetColor(label, Quantity_Color(0.2, 0.7, 0.3, Quantity_TOC_RGB), XCAFDoc_ColorGen);
    XCAFDoc_DocumentTool::SetLengthUnit(document, unitMeters);
    return WriteDocument(document, path, schema);
}

// A file authored in inches to prove non-millimetre unit handling.
bool WriteInchPart(const std::filesystem::path& path)
{
    Interface_Static::SetCVal("write.step.unit", "INCH");
    const bool result = WritePart(path, "AP214IS", 0.0254);
    Interface_Static::SetCVal("write.step.unit", "MM");
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : L"fixtures";
    std::error_code error;
    std::filesystem::create_directories(output, error);
    XCAFApp_Application::GetApplication();
    Interface_Static::SetCVal("write.step.unit", "MM");

    struct Case { const wchar_t* name; bool (*write)(const std::filesystem::path&); };
    const Case cases[] = {
        {L"assembly_ap214.stp", WriteAssembly},
        {L"part_ap203.stp", [](const std::filesystem::path& p) { return WritePart(p, "AP203"); }},
        {L"part_ap214.stp", [](const std::filesystem::path& p) { return WritePart(p, "AP214IS"); }},
        {L"part_ap242.stp", [](const std::filesystem::path& p) { return WritePart(p, "AP242DIS"); }},
        {L"inch_part_ap214.stp", WriteInchPart},
    };
    for (const auto& item : cases) {
        const auto path = output / item.name;
        if (!item.write(path)) {
            std::fprintf(stderr, "failed to write %ls\n", path.c_str());
            return 2;
        }
        std::printf("wrote %ls\n", path.c_str());
    }
    return 0;
}
