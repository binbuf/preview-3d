#include "IgesStreamReader.h"

#include <IGESData_FileRecognizer.hxx>
#include <IGESData_IGESReaderData.hxx>
#include <IGESData_IGESReaderTool.hxx>
#include <Interface_Check.hxx>
#include <Standard_Failure.hxx>

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The pinned OCCT C IGES record parser. These symbols live in TKDEIGES; the
// corresponding private header is not installed, so the small public surface
// IGESFile_Read itself uses is declared here.
extern "C" {
void iges_initfile(void);
void iges_finfile(int mode);
void iges_newparam(int typarg, int longval, char* parval);
void iges_param(int* Pstat, char* ligne, char c_separ, char c_fin, int lonlin);
void iges_Dsect(int* Dstat, int numsec, char* ligne);
void iges_Psect(int numsec, char ligne[80]);
void iges_setglobal(void);
void iges_stats(int* nbpart, int* nbparam);
void iges_nextpart(void);
int iges_lirparam(int* typarg, char** parval);
int iges_lirpart(int** tabval, char** res1, char** res2, char** nom, char** num, int* nbparam);
}

namespace step_host {
namespace {

// Parameter type tags produced by the pinned C parser (igesread.h).
enum : int {
    ArgVide = 0,
    ArgQuid = 1,
    ArgChar = 2,
    ArgInt = 3,
    ArgSign = 4,
    ArgReal = 5,
    ArgExp = 6,
    ArgRexp = 7,
    ArgMexp = 8,
};

using ParamTypes = std::array<Interface_ParamType, 10>;

const ParamTypes& Types()
{
    static const ParamTypes types = [] {
        ParamTypes value{};
        value[ArgVide] = Interface_ParamVoid;
        value[ArgQuid] = Interface_ParamMisc;
        value[ArgChar] = Interface_ParamText;
        value[ArgInt] = Interface_ParamInteger;
        value[ArgSign] = Interface_ParamInteger;
        value[ArgReal] = Interface_ParamReal;
        value[ArgExp] = Interface_ParamMisc;
        value[ArgRexp] = Interface_ParamReal;
        value[ArgMexp] = Interface_ParamEnum;
        return value;
    }();
    return types;
}

void ReadHeader(const Handle(IGESData_IGESReaderData)& data)
{
    char* parameter = nullptr;
    int type = 0;
    Standard_Integer last = 0;
    // Start section comments.
    while (iges_lirparam(&type, &parameter) != 0) {
        Standard_Integer j = 72;
        for (; j >= 0; --j) {
            if (parameter[j] > 32) break;
        }
        parameter[j + 1] = '\0';
        if (j >= 0 || last > 0) data->AddStartLine(parameter);
        ++last;
    }
    // Global section parameters.
    iges_setglobal();
    while (iges_lirparam(&type, &parameter) != 0)
        data->AddGlobal(Types()[type], parameter);
    data->SetGlobalSection();
}

void ReadContent(const Handle(IGESData_IGESReaderData)& data)
{
    char* res1 = nullptr;
    char* res2 = nullptr;
    char* name = nullptr;
    char* number = nullptr;
    char* parameter = nullptr;
    int* values = nullptr;
    int type = 0;
    int parameterCount = 0;
    int section = 0;
    while ((section = iges_lirpart(&values, &res1, &res2, &name, &number, &parameterCount)) != 0) {
        const int entity = (section + 1) / 2;
        data->SetDirPart(entity, values[0], values[1], values[2], values[3], values[4],
                         values[5], values[6], values[7], values[8], values[9], values[10],
                         values[11], values[12], values[13], values[14], values[15],
                         values[16], res1, res2, name, number);
        while (iges_lirparam(&type, &parameter) != 0) {
            if (type == ArgInt || type == ArgSign) {
                int referenced = std::atoi(parameter);
                if (referenced < 0) referenced = -referenced;
                referenced = (referenced & 1) ? (referenced + 1) / 2 : 0;
                data->AddParam(entity, parameter, Types()[type], referenced);
            } else {
                data->AddParam(entity, parameter, Types()[type]);
            }
        }
        data->InitParams(entity);
        iges_nextpart();
    }
}

} // namespace

Handle(IGESData_IGESModel) ReadIgesModelFromStream(
    std::istream& stream, const Handle(IGESData_Protocol)& protocol)
{
    int sections[6] = {0, 0, 0, 0, 0, 0};
    int directoryStatus = 0;
    int parameterStatus = 0;
    char separator = ',';
    char terminator = ';';
    iges_initfile();

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 80) {
            iges_finfile(0);
            return {};
        }
        const char sectionLetter = line[72];
        int section = 0;
        switch (sectionLetter) {
        case 'S': section = 1; break;
        case 'G': section = 2; break;
        case 'D': section = 3; break;
        case 'P': section = 4; break;
        case 'T': section = 5; break;
        default:
            iges_finfile(0);
            return {};
        }
        ++sections[section];
        const int number = sections[section];

        // The C parser expects a mutable NUL-terminated 72-column data field
        // with column 73 already cleared, exactly as iges_lire left it.
        std::array<char, 100> record{};
        std::memcpy(record.data(), line.data(), 80);
        record[72] = '\0';
        if (section == 1) {
            iges_newparam(0, 72, record.data());
        } else if (section == 2) {
            iges_setglobal();
            for (;;) {
                if (sections[2] == 1) {
                    int offset = 0;
                    if (record[0] != ',') {
                        separator = record[2];
                        offset = 3;
                    }
                    if (record[offset + 1] != separator) terminator = record[offset + 3];
                }
                iges_param(&parameterStatus, record.data(), separator, terminator, 72);
                if (parameterStatus != 2) break;
            }
        } else if (section == 3) {
            iges_Dsect(&directoryStatus, number, record.data());
        } else if (section == 4) {
            iges_Psect(number, record.data());
            for (;;) {
                iges_param(&parameterStatus, record.data(), separator, terminator, 64);
                if (parameterStatus != 2) break;
            }
        }
    }

    int parts = 0;
    int parameters = 0;
    iges_stats(&parts, &parameters);
    if (sections[5] == 0) {
        iges_finfile(0);
        return {};
    }

    Handle(IGESData_IGESReaderData) data = new IGESData_IGESReaderData(
        (sections[3] + 1) / 2, parameters);
    Handle(IGESData_IGESModel) model;
    try {
        ReadHeader(data);
        if (parts > 0) ReadContent(data);
        iges_finfile(1);

        model = new IGESData_IGESModel();
        Handle(IGESData_FileRecognizer) recognizer;
        IGESData_IGESReaderTool tool(data, protocol);
        tool.Prepare(recognizer);
        tool.SetErrorHandle(Standard_True);
        tool.LoadModel(model);
        if (model->Protocol().IsNull()) model->SetProtocol(protocol);
        if (model->NbEntities() <= 0) model.Nullify();
        iges_finfile(2);
    } catch (const Standard_Failure&) {
        iges_finfile(2);
        model.Nullify();
    } catch (...) {
        iges_finfile(2);
        model.Nullify();
    }
    return model;
}

} // namespace step_host
