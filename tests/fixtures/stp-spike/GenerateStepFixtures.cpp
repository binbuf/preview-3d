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
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
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

// A file authored in inches to prove non-millimetre unit handling. The writer
// expresses the authored unit in the STEP file; the pinned reader reports the
// normalized metre factor for the transferred geometry.
bool WriteInchPart(const std::filesystem::path& path)
{
    Interface_Static::SetCVal("write.step.unit", "INCH");
    const bool result = WritePart(path, "AP214IS");
    Interface_Static::SetCVal("write.step.unit", "MM");
    return result;
}

// STEP-003: a nested assembly (a sub-assembly reused twice) over the same box
// definition, to prove recursive occurrence transforms and cross-level
// definition reuse.
bool WriteNestedAssembly(const std::filesystem::path& path)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());

    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TDF_Label boxLabel = shapes->AddShape(box, Standard_False);
    colors->SetColor(boxLabel, Quantity_Color(0.8, 0.6, 0.2, Quantity_TOC_RGB), XCAFDoc_ColorGen);

    const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(2.0, 8.0).Shape();
    const TDF_Label cylinderLabel = shapes->AddShape(cylinder, Standard_False);
    colors->SetColor(cylinderLabel, Quantity_Color(0.2, 0.5, 0.8, Quantity_TOC_RGB), XCAFDoc_ColorGen);

    TopoDS_Compound subCompound;
    BRep_Builder subBuilder;
    subBuilder.MakeCompound(subCompound);
    const TDF_Label subAssembly = shapes->AddShape(subCompound, Standard_True);
    gp_Trsf cylinderOffset;
    cylinderOffset.SetTranslation(gp_Vec(10.0, 0.0, 0.0));
    shapes->AddComponent(subAssembly, boxLabel, TopLoc_Location());
    shapes->AddComponent(subAssembly, cylinderLabel, TopLoc_Location(cylinderOffset));

    TopoDS_Compound rootCompound;
    BRep_Builder rootBuilder;
    rootBuilder.MakeCompound(rootCompound);
    const TDF_Label rootAssembly = shapes->AddShape(rootCompound, Standard_True);

    gp_Trsf first;
    first.SetTranslation(gp_Vec(0.0, 0.0, 0.0));
    gp_Trsf second;
    second.SetTranslation(gp_Vec(0.0, 30.0, 0.0));
    shapes->AddComponent(rootAssembly, subAssembly, TopLoc_Location(first));
    shapes->AddComponent(rootAssembly, subAssembly, TopLoc_Location(second));
    shapes->AddComponent(rootAssembly, boxLabel, TopLoc_Location(gp_Trsf()));
    shapes->UpdateAssemblies();
    return WriteDocument(document, path, "AP214IS");
}

// STEP-003: one shared definition placed three times; the middle occurrence
// carries a component (instance) color that must override the definition color
// without duplicating the reusable geometry.
bool WriteInstanceColorAssembly(const std::filesystem::path& path)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());

    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TDF_Label boxLabel = shapes->AddShape(box, Standard_False);
    colors->SetColor(boxLabel, Quantity_Color(0.7, 0.1, 0.1, Quantity_TOC_RGB), XCAFDoc_ColorGen);

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const TDF_Label assembly = shapes->AddShape(compound, Standard_True);

    for (int i = 0; i < 3; ++i) {
        gp_Trsf placement;
        placement.SetTranslation(gp_Vec(double(i) * 20.0, 0.0, 0.0));
        shapes->AddComponent(assembly, boxLabel, TopLoc_Location(placement));
    }
    shapes->UpdateAssemblies();

    TDF_LabelSequence components;
    shapes->GetComponents(assembly, components);
    if (components.Length() >= 2) {
        const TopoDS_Shape middle = XCAFDoc_ShapeTool::GetShape(components.Value(2));
        colors->SetInstanceColor(middle, XCAFDoc_ColorGen,
                                 Quantity_Color(0.1, 0.8, 0.2, Quantity_TOC_RGB));
    }
    return WriteDocument(document, path, "AP214IS");
}

// STEP-003: a definition with per-face subshape colors and no shape-level
// color, forcing a bounded material seam split of the reusable geometry.
bool WriteFaceColorPart(const std::filesystem::path& path)
{
    const Handle(TDocStd_Document) document = NewDocument();
    const Handle(XCAFDoc_ShapeTool) shapes = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colors = XCAFDoc_DocumentTool::ColorTool(document->Main());

    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TDF_Label boxLabel = shapes->AddShape(box, Standard_False);

    int faceIndex = 0;
    for (TopExp_Explorer explorer(box, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TDF_Label sub = shapes->AddSubShape(boxLabel, TopoDS::Face(explorer.Current()));
        if (sub.IsNull()) continue;
        const Quantity_Color color = (faceIndex++ % 2 == 0)
            ? Quantity_Color(0.9, 0.2, 0.1, Quantity_TOC_RGB)
            : Quantity_Color(0.1, 0.3, 0.9, Quantity_TOC_RGB);
        colors->SetColor(sub, color, XCAFDoc_ColorSurf);
    }
    return WriteDocument(document, path, "AP214IS");
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
        {L"assembly_nested_ap214.stp", WriteNestedAssembly},
        {L"instance_color_ap214.stp", WriteInstanceColorAssembly},
        {L"face_color_ap214.stp", WriteFaceColorPart},
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
