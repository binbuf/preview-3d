#include "FcstdReader.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <TDF_LabelSequence.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace step_host {
namespace {

// --- bounded XML reader -----------------------------------------------------

struct XmlAttribute {
    std::string name;
    std::string value;
};

struct XmlNode {
    std::string name;
    std::vector<XmlAttribute> attributes;
    // unique_ptr keeps an open child's address stable while siblings are
    // appended; a plain vector would reallocate and invalidate the parse stack.
    std::vector<std::unique_ptr<XmlNode>> children;
    std::string text;
};

bool IsSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool IsNameStart(char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_'
        || value == ':';
}

bool IsNameChar(char value)
{
    return IsNameStart(value) || (value >= '0' && value <= '9') || value == '-' || value == '.';
}

bool IsForbiddenControl(char value)
{
    const auto byte = static_cast<unsigned char>(value);
    return byte < 0x20 && value != '\t' && value != '\n' && value != '\r';
}

bool IsSafeText(std::string_view text)
{
    for (const char value : text)
        if (IsForbiddenControl(value) || value == '\0') return false;
    return true;
}

std::string UnescapeXml(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '&') {
            result.push_back(text[index]);
            continue;
        }
        const std::size_t semi = text.find(';', index + 1);
        if (semi == std::string_view::npos || semi - index > 12) {
            result.push_back('&');
            continue;
        }
        const std::string_view entity = text.substr(index + 1, semi - index - 1);
        if (entity == "amp") result.push_back('&');
        else if (entity == "lt") result.push_back('<');
        else if (entity == "gt") result.push_back('>');
        else if (entity == "quot") result.push_back('"');
        else if (entity == "apos") result.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            unsigned long value = 0;
            try {
                if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                    value = std::stoul(std::string(entity.substr(2)), nullptr, 16);
                else
                    value = std::stoul(std::string(entity.substr(1)), nullptr, 10);
            } catch (...) {
                return {};
            }
            if (value == 0 || value > 0x10ffff) return {};
            if (value < 0x80) result.push_back(static_cast<char>(value));
            else result.push_back('?'); // bounded fallback for non-ASCII entities
        } else {
            result.push_back('&');
            continue;
        }
        index = semi;
    }
    return result;
}

class XmlParser {
public:
    XmlParser(std::string_view text, const FcstdLimits& limits)
        : text_(text), limits_(limits)
    {
    }

    bool Parse(XmlNode& root)
    {
        if (text_.size() > limits_.maxXmlBytes) return false;
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xef
            && static_cast<unsigned char>(text_[1]) == 0xbb
            && static_cast<unsigned char>(text_[2]) == 0xbf)
            index_ = 3;

        std::vector<XmlNode*> stack;
        while (index_ < text_.size()) {
            if (Cancelled()) return false;
            if (text_[index_] != '<') {
                const std::size_t start = index_;
                while (index_ < text_.size() && text_[index_] != '<') ++index_;
                const std::string_view raw = text_.substr(start, index_ - start);
                if (!IsSafeText(raw)) return false;
                if (!stack.empty()) {
                    std::string value = UnescapeXml(raw);
                    if (stack.back()->text.size() + value.size() > 4u * 1024 * 1024) return false;
                    stack.back()->text += value;
                }
                continue;
            }
            if (text_.compare(index_, 4, "<!--") == 0) {
                const std::size_t end = text_.find("-->", index_ + 4);
                if (end == std::string_view::npos) return false;
                index_ = end + 3;
                continue;
            }
            if (text_.compare(index_, 2, "<?") == 0) {
                const std::size_t end = text_.find("?>", index_ + 2);
                if (end == std::string_view::npos) return false;
                index_ = end + 2;
                continue;
            }
            if (text_.compare(index_, 9, "<![CDATA[") == 0) {
                const std::size_t end = text_.find("]]>", index_ + 9);
                if (end == std::string_view::npos) return false;
                if (!stack.empty()) {
                    const std::string_view raw = text_.substr(index_ + 9, end - index_ - 9);
                    if (!IsSafeText(raw)) return false;
                    if (stack.back()->text.size() + raw.size() > 4u * 1024 * 1024) return false;
                    stack.back()->text.append(raw);
                }
                index_ = end + 3;
                continue;
            }
            if (text_.compare(index_, 2, "<!") == 0) {
                // DOCTYPE/ENTITY declarations are rejected outright: FCStd has
                // no legitimate use for them and they are the XXE surface.
                return false;
            }
            if (text_.compare(index_, 2, "</") == 0) {
                index_ += 2;
                const std::string name = ParseName();
                if (name.empty() || stack.empty() || stack.back()->name != name) return false;
                SkipSpace();
                if (index_ >= text_.size() || text_[index_] != '>') return false;
                ++index_;
                stack.pop_back();
                continue;
            }

            ++index_; // '<'
            const std::string name = ParseName();
            if (name.empty()) return false;
            if (++nodeCount_ > limits_.maxXmlNodes) return false;
            XmlNode node;
            node.name = name;
            if (!ParseAttributes(node)) return false;
            SkipSpace();
            bool selfClosing = false;
            if (index_ < text_.size() && text_[index_] == '/') {
                selfClosing = true;
                ++index_;
            }
            if (index_ >= text_.size() || text_[index_] != '>') return false;
            ++index_;

            XmlNode* placed = nullptr;
            if (stack.empty()) {
                if (rootPlaced_) return false;
                root = std::move(node);
                rootPlaced_ = true;
                placed = &root;
            } else {
                stack.back()->children.push_back(std::make_unique<XmlNode>(std::move(node)));
                placed = stack.back()->children.back().get();
            }
            if (!selfClosing) {
                if (stack.size() + 1 > limits_.maxXmlDepth) return false;
                stack.push_back(placed);
            }
        }
        return stack.empty() && rootPlaced_;
    }

    bool cancelled = false;

private:
    bool Cancelled() { return cancelled; }

    void SkipSpace()
    {
        while (index_ < text_.size() && IsSpace(text_[index_])) ++index_;
    }

    std::string ParseName()
    {
        if (index_ >= text_.size() || !IsNameStart(text_[index_])) return {};
        const std::size_t start = index_;
        while (index_ < text_.size() && IsNameChar(text_[index_])) ++index_;
        return std::string(text_.substr(start, index_ - start));
    }

    bool ParseAttributes(XmlNode& node)
    {
        for (;;) {
            SkipSpace();
            if (index_ >= text_.size()) return false;
            if (text_[index_] == '>' || (text_[index_] == '/' && index_ + 1 < text_.size()
                                         && text_[index_ + 1] == '>'))
                return true;
            const std::string name = ParseName();
            if (name.empty()) return false;
            SkipSpace();
            if (index_ >= text_.size() || text_[index_] != '=') return false;
            ++index_;
            SkipSpace();
            if (index_ >= text_.size() || (text_[index_] != '"' && text_[index_] != '\''))
                return false;
            const char quote = text_[index_++];
            const std::size_t start = index_;
            while (index_ < text_.size() && text_[index_] != quote) {
                if (IsForbiddenControl(text_[index_])) return false;
                ++index_;
            }
            if (index_ >= text_.size()) return false;
            const std::string value = UnescapeXml(text_.substr(start, index_ - start));
            ++index_;
            if (value.size() > 1024 * 1024) return false;
            if (node.attributes.size() >= 64) return false;
            node.attributes.push_back(XmlAttribute{name, value});
        }
    }

    std::string_view text_;
    const FcstdLimits& limits_;
    std::size_t index_ = 0;
    std::uint32_t nodeCount_ = 0;
    bool rootPlaced_ = false;
};

const XmlNode* FindChild(const XmlNode& node, std::string_view name)
{
    for (const auto& child : node.children)
        if (child->name == name) return child.get();
    return nullptr;
}

std::vector<const XmlNode*> FindChildren(const XmlNode& node, std::string_view name)
{
    std::vector<const XmlNode*> result;
    for (const auto& child : node.children)
        if (child->name == name) result.push_back(child.get());
    return result;
}

const std::string* Attribute(const XmlNode& node, std::string_view name)
{
    for (const auto& attribute : node.attributes)
        if (attribute.name == name) return &attribute.value;
    return nullptr;
}

bool ParseDouble(std::string_view text, double& value)
{
    std::string copy(text);
    if (copy.empty()) return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtod(copy.c_str(), &end);
    return end && *end == '\0' && std::isfinite(value);
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value)
{
    if (text.empty()) return false;
    value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - 9) / 10) return false;
        value = value * 10 + std::uint64_t(ch - '0');
    }
    return true;
}

bool ParsePlacement(const XmlNode& node, gp_Trsf& trsf)
{
    trsf = gp_Trsf();
    double px = 0, py = 0, pz = 0, q0 = 0, q1 = 0, q2 = 0, q3 = 1;
    const auto read = [&node](const char* name, double& out) {
        const std::string* value = Attribute(node, name);
        if (!value) return true;
        return ParseDouble(*value, out);
    };
    if (!read("Px", px) || !read("Py", py) || !read("Pz", pz) || !read("Q0", q0)
        || !read("Q1", q1) || !read("Q2", q2) || !read("Q3", q3))
        return false;
    gp_Quaternion quaternion(q0, q1, q2, q3);
    if (quaternion.SquareNorm() > 0.0) trsf.SetRotation(quaternion);
    trsf.SetTranslationPart(gp_Vec(px, py, pz));
    return true;
}

bool SafePackageReference(std::string_view reference)
{
    if (reference.empty() || reference.size() > 512) return false;
    if (reference.front() == '/' || reference.front() == '\\' || reference.front() == '.')
        return false;
    if (reference.find(':') != std::string_view::npos
        || reference.find('\\') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= reference.size()) {
        const std::size_t slash = reference.find('/', start);
        const std::string_view component = reference.substr(
            start, slash == std::string_view::npos ? reference.size() - start : slash - start);
        if (component.empty() || component == "." || component == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    for (const char value : reference)
        if (IsForbiddenControl(value)) return false;
    return true;
}

std::string Fold(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

// --- FreeCAD object model ---------------------------------------------------

struct FcColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

FcColor UnpackRgb(std::uint32_t packed)
{
    return FcColor{static_cast<std::uint8_t>((packed >> 24) & 0xff),
                   static_cast<std::uint8_t>((packed >> 16) & 0xff),
                   static_cast<std::uint8_t>((packed >> 8) & 0xff), 255};
}

struct FcObject {
    std::string name;
    std::string label;
    std::string type;
    std::string shapeFile;
    gp_Trsf placement;
    gp_Trsf linkPlacement;
    bool hasVisible = false;
    bool visible = true;
    bool linked = false;
    bool linkedExternal = false;
    bool linkTransform = false;
    std::string linkedObject;
    std::string baseLink;
    std::string placementListFile;
    std::vector<std::string> children;
    std::string parent;
    std::optional<FcColor> color;
    std::vector<FcColor> faceColors;
    int transparency = 0;

    bool IsGeoFeatureGroup() const
    {
        return type == "App::Part" || type == "Assembly::AssemblyObject";
    }
};

struct DocumentModel {
    std::unordered_map<std::string, FcObject> objects;
    std::vector<std::string> order;
};

void ParseObjectProperties(const XmlNode& properties, FcObject& object)
{
    for (const XmlNode* property : FindChildren(properties, "Property")) {
        const std::string* name = Attribute(*property, "name");
        if (!name) continue;
        if (*name == "Shape") {
            if (const XmlNode* part = FindChild(*property, "Part")) {
                if (const std::string* file = Attribute(*part, "file"); file && !file->empty())
                    object.shapeFile = *file;
            }
        } else if (*name == "Label") {
            if (const XmlNode* text = FindChild(*property, "String")) {
                if (const std::string* value = Attribute(*text, "value"); value && !value->empty())
                    object.label = *value;
            }
        } else if (*name == "Placement") {
            if (const XmlNode* placement = FindChild(*property, "PropertyPlacement"))
                ParsePlacement(*placement, object.placement);
        } else if (*name == "Visibility") {
            if (const XmlNode* boolean = FindChild(*property, "Bool")) {
                if (const std::string* value = Attribute(*boolean, "value")) {
                    object.hasVisible = true;
                    object.visible = *value == "true" || *value == "1";
                }
            }
        } else if (*name == "LinkedObject") {
            if (const XmlNode* link = FindChild(*property, "XLink")) {
                if (const std::string* value = Attribute(*link, "name"); value && !value->empty())
                    object.linkedObject = *value;
                const std::string* file = Attribute(*link, "file");
                object.linkedExternal = file && !file->empty();
                object.linked = !object.linkedObject.empty();
            }
        } else if (*name == "LinkPlacement") {
            if (const XmlNode* placement = FindChild(*property, "PropertyPlacement"))
                ParsePlacement(*placement, object.linkPlacement);
        } else if (*name == "LinkTransform") {
            if (const XmlNode* boolean = FindChild(*property, "Bool")) {
                if (const std::string* value = Attribute(*boolean, "value"))
                    object.linkTransform = *value == "true" || *value == "1";
            }
        } else if (*name == "Base") {
            if (const XmlNode* link = FindChild(*property, "Link")) {
                if (const std::string* value = Attribute(*link, "value"); value && !value->empty())
                    object.baseLink = *value;
            }
        } else if (*name == "PlacementList") {
            if (const XmlNode* list = FindChild(*property, "PlacementList")) {
                if (const std::string* file = Attribute(*list, "file"); file && !file->empty())
                    object.placementListFile = *file;
            }
        } else if (*name == "Group") {
            if (const XmlNode* list = FindChild(*property, "LinkList")) {
                for (const XmlNode* link : FindChildren(*list, "Link")) {
                    if (const std::string* value = Attribute(*link, "value");
                        value && !value->empty())
                        object.children.push_back(*value);
                }
            }
        }
    }
}

bool ParseDocumentModel(const XmlNode& root, const FcstdLimits& limits, DocumentModel& model,
                        std::uint32_t& warningCount, model_core::CompatibilityReportBuilder& compatibility)
{
    if (root.name != "Document") return false;
    const XmlNode* objects = FindChild(root, "Objects");
    if (!objects) return false;
    for (const XmlNode* element : FindChildren(*objects, "Object")) {
        const std::string* name = Attribute(*element, "name");
        const std::string* type = Attribute(*element, "type");
        if (!name || !type || name->empty()) continue;
        if (model.objects.size() >= limits.maxObjects) {
            return false;
        }
        FcObject object;
        object.name = *name;
        object.label = *name;
        object.type = *type;
        if (model.objects.emplace(object.name, std::move(object)).second)
            model.order.push_back(*name);
    }
    if (const XmlNode* objectData = FindChild(root, "ObjectData")) {
        for (const XmlNode* element : FindChildren(*objectData, "Object")) {
            const std::string* name = Attribute(*element, "name");
            if (!name) continue;
            const auto found = model.objects.find(*name);
            if (found == model.objects.end()) continue;
            if (const XmlNode* properties = FindChild(*element, "Properties"))
                ParseObjectProperties(*properties, found->second);
        }
    }
    for (const auto& [name, object] : model.objects) {
        for (const std::string& child : object.children) {
            const auto found = model.objects.find(child);
            if (found != model.objects.end()) found->second.parent = name;
        }
    }
    (void)warningCount;
    (void)compatibility;
    return true;
}

void ParseGuiProperties(const XmlNode& properties, FcObject& object)
{
    for (const XmlNode* property : FindChildren(properties, "Property")) {
        const std::string* name = Attribute(*property, "name");
        if (!name) continue;
        if (*name == "Visibility") {
            if (!object.hasVisible) {
                if (const XmlNode* boolean = FindChild(*property, "Bool")) {
                    if (const std::string* value = Attribute(*boolean, "value")) {
                        object.hasVisible = true;
                        object.visible = *value == "true" || *value == "1";
                    }
                }
            }
        } else if (*name == "ShapeColor") {
            if (const XmlNode* color = FindChild(*property, "PropertyColor")) {
                std::uint64_t packed = 0;
                if (const std::string* value = Attribute(*color, "value");
                    value && ParseUnsigned(*value, packed) && packed <= 0xffffffffull)
                    object.color = UnpackRgb(static_cast<std::uint32_t>(packed));
            }
        } else if (*name == "Transparency") {
            if (const XmlNode* integer = FindChild(*property, "Integer")) {
                std::uint64_t value = 0;
                if (const std::string* text = Attribute(*integer, "value");
                    text && ParseUnsigned(*text, value))
                    object.transparency = static_cast<int>((std::min)(std::uint64_t(100), value));
            }
        } else if (*name == "DiffuseColor") {
            if (const XmlNode* list = FindChild(*property, "ColorList")) {
                // The color-list file is resolved by the caller once the entry
                // catalog is available; remember it via the transparency field's
                // sibling handled in ParseGuiDocument.
            }
        }
    }
}

bool ParseGuiDocument(const XmlNode& root, DocumentModel& model)
{
    if (root.name != "Document") return false;
    const XmlNode* viewProviders = FindChild(root, "ViewProviderData");
    if (!viewProviders) return false;
    for (const XmlNode* provider : FindChildren(*viewProviders, "ViewProvider")) {
        const std::string* name = Attribute(*provider, "name");
        if (!name) continue;
        const auto found = model.objects.find(*name);
        if (found == model.objects.end()) continue;
        if (const XmlNode* properties = FindChild(*provider, "Properties"))
            ParseGuiProperties(*properties, found->second);
    }
    return true;
}

bool ReadXmlEntry(const FcstdPackage& package, std::span<const std::byte> bytes,
                  std::string_view foldedName, const FcstdLimits& limits, XmlNode& root,
                  std::uint32_t& warningCount)
{
    const FcstdEntry* entry = FindFcstdEntry(package, foldedName);
    if (!entry) return false;
    std::vector<std::byte> content;
    if (ExtractFcstdEntry(bytes, *entry, content, limits.maxXmlBytes)
        != FcstdPreflightStatus::Ok) {
        ++warningCount;
        return false;
    }
    std::string text(reinterpret_cast<const char*>(content.data()), content.size());
    XmlParser parser(text, limits);
    return parser.Parse(root);
}

bool ReadPlacements(const FcstdPackage& package, std::span<const std::byte> bytes,
                    const std::string& file, const FcstdLimits& limits,
                    std::vector<gp_Trsf>& placements)
{
    placements.clear();
    if (!SafePackageReference(file)) return false;
    const FcstdEntry* entry = FindFcstdEntry(package, Fold(file));
    if (!entry) return false;
    std::vector<std::byte> content;
    if (ExtractFcstdEntry(bytes, *entry, content, limits.maxEntryBytes)
        != FcstdPreflightStatus::Ok)
        return false;
    if (content.size() < sizeof(std::uint32_t)) return false;
    std::uint32_t count = 0;
    std::memcpy(&count, content.data(), sizeof(count));
    constexpr std::size_t kPlacementBytes = 7 * sizeof(double);
    if (count > limits.maxObjects
        || content.size() < sizeof(std::uint32_t) + kPlacementBytes * std::size_t(count))
        return false;
    placements.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::byte* base = content.data() + sizeof(std::uint32_t) + kPlacementBytes * index;
        double values[7];
        std::memcpy(values, base, sizeof(values));
        for (const double value : values)
            if (!std::isfinite(value)) return false;
        gp_Quaternion quaternion(values[3], values[4], values[5], values[6]);
        gp_Trsf trsf;
        if (quaternion.SquareNorm() > 0.0) trsf.SetRotation(quaternion);
        trsf.SetTranslationPart(gp_Vec(values[0], values[1], values[2]));
        placements.push_back(trsf);
    }
    return true;
}

bool ReadColorList(const FcstdPackage& package, std::span<const std::byte> bytes,
                   const std::string& file, const FcstdLimits& limits,
                   FcObject& object)
{
    if (!SafePackageReference(file)) return false;
    const FcstdEntry* entry = FindFcstdEntry(package, Fold(file));
    if (!entry) return false;
    std::vector<std::byte> content;
    if (ExtractFcstdEntry(bytes, *entry, content, limits.maxEntryBytes)
        != FcstdPreflightStatus::Ok)
        return false;
    if (content.size() < sizeof(std::uint32_t)) return false;
    std::uint32_t count = 0;
    std::memcpy(&count, content.data(), sizeof(count));
    if (count > limits.maxObjects * 64u
        || content.size() < sizeof(std::uint32_t) + std::size_t(count) * sizeof(std::uint32_t))
        return false;
    std::vector<FcColor> colors;
    colors.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t packed = 0;
        std::memcpy(&packed, content.data() + sizeof(std::uint32_t)
                              + std::size_t(index) * sizeof(std::uint32_t),
                    sizeof(packed));
        colors.push_back(UnpackRgb(packed));
    }
    if (colors.size() == 1) object.color = colors.front();
    else if (colors.size() > 1) object.faceColors = std::move(colors);
    return true;
}

bool AncestorsPlacement(const DocumentModel& model, const FcObject& object, gp_Trsf& result,
                        std::uint32_t maxDepth)
{
    result = gp_Trsf();
    const FcObject* current = &object;
    for (std::uint32_t depth = 0; !current->parent.empty() && depth < maxDepth; ++depth) {
        const auto found = model.objects.find(current->parent);
        if (found == model.objects.end()) return false;
        current = &found->second;
        if (current->IsGeoFeatureGroup()) result = current->placement * result;
    }
    return true;
}

bool EffectivelyVisible(const DocumentModel& model, const FcObject& object, std::uint32_t maxDepth)
{
    if (!object.visible) return false;
    const FcObject* current = &object;
    for (std::uint32_t depth = 0; !current->parent.empty() && depth < maxDepth; ++depth) {
        const auto found = model.objects.find(current->parent);
        if (found == model.objects.end()) break;
        current = &found->second;
        if (current->IsGeoFeatureGroup() && !current->visible) return false;
    }
    return true;
}

bool HasVisibleGeometryInSubtree(const DocumentModel& model, const FcObject& object,
                                 std::uint32_t maxDepth, std::uint32_t depth = 0)
{
    if (depth >= maxDepth) return false;
    for (const std::string& child : object.children) {
        const auto found = model.objects.find(child);
        if (found == model.objects.end()) continue;
        if (!found->second.shapeFile.empty()
            && EffectivelyVisible(model, found->second, maxDepth))
            return true;
        if (HasVisibleGeometryInSubtree(model, found->second, maxDepth, depth + 1)) return true;
    }
    return false;
}

// --- bounded BRep/shape loading --------------------------------------------

class ShapeCache {
public:
    ShapeCache(const FcstdPackage& package, std::span<const std::byte> bytes,
               const FcstdLimits& limits)
        : package_(package), bytes_(bytes), limits_(limits)
    {
    }

    bool Load(const std::string& file, TopoDS_Shape& shape)
    {
        if (!SafePackageReference(file)) return false;
        const std::string key = Fold(file);
        const auto cached = shapes_.find(key);
        if (cached != shapes_.end()) {
            shape = cached->second;
            return !shape.IsNull();
        }
        const FcstdEntry* entry = FindFcstdEntry(package_, key);
        if (!entry) return false;
        std::vector<std::byte> content;
        if (ExtractFcstdEntry(bytes_, *entry, content, limits_.maxEntryBytes)
            != FcstdPreflightStatus::Ok)
            return false;
        std::string data(reinterpret_cast<const char*>(content.data()), content.size());
        std::istringstream stream(data);
        TopoDS_Shape loaded;
        BRep_Builder builder;
        try {
            BRepTools::Read(loaded, stream, builder);
        } catch (...) {
            return false;
        }
        if (loaded.IsNull()) return false;
        shapes_.emplace(key, loaded);
        shape = loaded;
        return true;
    }

private:
    const FcstdPackage& package_;
    std::span<const std::byte> bytes_;
    const FcstdLimits& limits_;
    std::unordered_map<std::string, TopoDS_Shape> shapes_;
};

} // namespace

FcstdReadResult BuildFcstdDocument(const FcstdPackage& package,
                                   std::span<const std::byte> bytes,
                                   const Handle(TDocStd_Document)& document,
                                   const FcstdLimits& limits,
                                   model_core::CompatibilityReportBuilder& compatibility,
                                   const std::function<bool()>& isCancelled)
{
    FcstdReadResult result;
    if (document.IsNull()) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }
    if (isCancelled && isCancelled()) {
        result.errorCode = model_core::ImportErrorCode::Cancelled;
        return result;
    }

    XmlNode documentXml;
    if (!ReadXmlEntry(package, bytes, "document.xml", limits, documentXml, result.warningCount)) {
        result.errorCode = model_core::ImportErrorCode::MalformedData;
        return result;
    }
    DocumentModel model;
    if (!ParseDocumentModel(documentXml, limits, model, result.warningCount, compatibility)) {
        result.errorCode = model_core::ImportErrorCode::MalformedData;
        return result;
    }
    if (model.objects.empty()) {
        result.errorCode = model_core::ImportErrorCode::EmptyGeometry;
        return result;
    }

    XmlNode guiXml;
    if (ReadXmlEntry(package, bytes, "guidocument.xml", limits, guiXml, result.warningCount))
        ParseGuiDocument(guiXml, model);

    // The color-list file name lives on DiffuseColor properties nested in the
    // GuiDocument tree; read them after the object map is complete.
    if (const XmlNode* viewProviders = FindChild(guiXml, "ViewProviderData")) {
        for (const XmlNode* provider : FindChildren(*viewProviders, "ViewProvider")) {
            const std::string* name = Attribute(*provider, "name");
            if (!name) continue;
            const auto found = model.objects.find(*name);
            if (found == model.objects.end()) continue;
            const XmlNode* properties = FindChild(*provider, "Properties");
            if (!properties) continue;
            for (const XmlNode* property : FindChildren(*properties, "Property")) {
                const std::string* propertyName = Attribute(*property, "name");
                if (!propertyName || *propertyName != "DiffuseColor") continue;
                const XmlNode* list = FindChild(*property, "ColorList");
                if (!list) continue;
                const std::string* file = Attribute(*list, "file");
                if (!file || file->empty()) continue;
                if (!ReadColorList(package, bytes, *file, limits, found->second))
                    ++result.warningCount;
            }
        }
    }

    // FCStd has no authored unit: FreeCAD's evaluated Part payload is stored
    // in millimetres. Name the assumption on the compatibility channel.
    if (compatibility.Add(model_core::kCadFeatureFcstdMillimetresAssumed,
                          model_core::CompatibilitySeverity::Ignored))
        ++result.warningCount;

    ShapeCache cache(package, bytes, limits);
    std::uint32_t unsupported = 0;
    std::uint32_t externalLinks = 0;
    std::vector<TopoDS_Shape> shapes;
    std::vector<std::uint32_t> instanceObjectIndex;
    std::vector<std::pair<std::size_t, gp_Trsf>> instancePlacements;
    std::vector<FcObject> shapeOwners;

    for (const std::string& name : model.order) {
        if (isCancelled && isCancelled()) {
            result.errorCode = model_core::ImportErrorCode::Cancelled;
            return result;
        }
        const auto found = model.objects.find(name);
        if (found == model.objects.end()) continue;
        const FcObject& object = found->second;
        if (!EffectivelyVisible(model, object, 64)) continue;
        if (!object.shapeFile.empty() && !object.children.empty()
            && HasVisibleGeometryInSubtree(model, object, 64))
            continue; // container's aggregated copy; render the children only

        gp_Trsf own = object.placement;
        const FcObject* geometryOwner = &object;
        std::vector<gp_Trsf> arrayPlacements;
        if (object.shapeFile.empty() && object.linked) {
            if (object.linkedExternal) {
                ++externalLinks;
                continue;
            }
            const auto target = model.objects.find(object.linkedObject);
            if (target == model.objects.end() || target->second.shapeFile.empty()) {
                ++unsupported;
                continue;
            }
            geometryOwner = &target->second;
            own = object.linkTransform ? object.linkPlacement * target->second.placement
                                       : object.linkPlacement;
        } else if (object.shapeFile.empty() && !object.baseLink.empty()
                   && !object.placementListFile.empty()) {
            const auto target = model.objects.find(object.baseLink);
            if (target == model.objects.end() || target->second.shapeFile.empty()) {
                ++unsupported;
                continue;
            }
            if (!ReadPlacements(package, bytes, object.placementListFile, limits, arrayPlacements)) {
                ++unsupported;
                continue;
            }
            geometryOwner = &target->second;
        } else if (object.shapeFile.empty()) {
            continue; // pure container
        }

        TopoDS_Shape shape;
        if (!cache.Load(geometryOwner->shapeFile, shape)) {
            ++unsupported;
            continue;
        }
        gp_Trsf ancestors;
        if (!AncestorsPlacement(model, object, ancestors, 64)) {
            ++unsupported;
            continue;
        }

        const std::size_t ownerIndex = shapeOwners.size();
        shapeOwners.push_back(*geometryOwner);
        // Preserve the object's own color override, falling back to the
        // geometry owner for links.
        if (!object.color.has_value() && geometryOwner->color.has_value())
            shapeOwners.back().color = geometryOwner->color;
        if (object.faceColors.empty() && !geometryOwner->faceColors.empty())
            shapeOwners.back().faceColors = geometryOwner->faceColors;
        if (object.transparency == 0) shapeOwners.back().transparency = geometryOwner->transparency;

        if (!arrayPlacements.empty()) {
            for (const gp_Trsf& placement : arrayPlacements) {
                const gp_Trsf world = ancestors * (object.placement * placement);
                shapes.push_back(shape);
                instanceObjectIndex.push_back(static_cast<std::uint32_t>(ownerIndex));
                instancePlacements.emplace_back(ownerIndex, world);
            }
        } else {
            const gp_Trsf world = ancestors * own;
            shapes.push_back(shape);
            instanceObjectIndex.push_back(static_cast<std::uint32_t>(ownerIndex));
            instancePlacements.emplace_back(ownerIndex, world);
        }
    }

    if (externalLinks > 0
        && compatibility.Add(model_core::kCadFeatureFcstdExternalLinkIgnored,
                             model_core::CompatibilitySeverity::Degraded, externalLinks))
        ++result.warningCount;
    if (unsupported > 0
        && compatibility.Add(model_core::kCadFeatureFcstdUnsupportedObject,
                             model_core::CompatibilitySeverity::Ignored, unsupported))
        ++result.warningCount;
    if (instancePlacements.empty()) {
        result.errorCode = model_core::ImportErrorCode::EmptyGeometry;
        return result;
    }

    const Handle(XCAFDoc_ShapeTool) shapeTool =
        XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colorTool =
        XCAFDoc_DocumentTool::ColorTool(document->Main());
    if (shapeTool.IsNull() || colorTool.IsNull()) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    const TDF_Label root = shapeTool->AddShape(compound, Standard_True);
    if (root.IsNull()) {
        result.errorCode = model_core::ImportErrorCode::InternalImporterFailure;
        return result;
    }

    // Add one definition per distinct source shape file / owner, then place it.
    std::unordered_map<std::string, TDF_Label> definitionByOwner;
    for (const auto& [ownerIndex, world] : instancePlacements) {
        const FcObject& owner = shapeOwners[ownerIndex];
        TDF_Label definition;
        const auto existing = definitionByOwner.find(owner.shapeFile);
        if (existing != definitionByOwner.end()) {
            definition = existing->second;
        } else {
            const auto cachedShape = std::find_if(
                instanceObjectIndex.begin(), instanceObjectIndex.end(),
                [ownerIndex](std::uint32_t value) { return value == ownerIndex; });
            if (cachedShape == instanceObjectIndex.end()) continue;
            const std::size_t shapeIndex = std::size_t(cachedShape - instanceObjectIndex.begin());
            definition = shapeTool->AddShape(shapes[shapeIndex], Standard_False);
            if (definition.IsNull()) continue;
            const int transparency = (std::max)(0, (std::min)(100, owner.transparency));
            const std::uint8_t alpha = static_cast<std::uint8_t>(
                255 * (100 - transparency) / 100);
            if (owner.color.has_value()) {
                const FcColor& color = *owner.color;
                colorTool->SetColor(definition,
                                    Quantity_ColorRGBA(static_cast<float>(color.r / 255.0),
                                                       static_cast<float>(color.g / 255.0),
                                                       static_cast<float>(color.b / 255.0),
                                                       static_cast<float>(alpha / 255.0)),
                                    XCAFDoc_ColorGen);
            }
            if (!owner.faceColors.empty()) {
                TopExp_Explorer explorer(shapes[shapeIndex], TopAbs_FACE);
                std::size_t faceIndex = 0;
                for (; explorer.More() && faceIndex < owner.faceColors.size();
                     explorer.Next(), ++faceIndex) {
                    const TopoDS_Shape& face = explorer.Current();
                    TDF_Label subShape;
                    if (shapeTool->AddSubShape(definition, face, subShape) && !subShape.IsNull()) {
                        const FcColor& color = owner.faceColors[faceIndex];
                        colorTool->SetColor(subShape,
                                            Quantity_ColorRGBA(static_cast<float>(color.r / 255.0),
                                                               static_cast<float>(color.g / 255.0),
                                                               static_cast<float>(color.b / 255.0),
                                                               static_cast<float>(alpha / 255.0)),
                                            XCAFDoc_ColorSurf);
                    }
                }
            }
            definitionByOwner.emplace(owner.shapeFile, definition);
        }
        if (!definition.IsNull())
            shapeTool->AddComponent(root, definition, TopLoc_Location(world));
    }
    shapeTool->UpdateAssemblies();

    result.visibleShapeCount = static_cast<std::uint32_t>(instancePlacements.size());
    return result;
}

} // namespace step_host
