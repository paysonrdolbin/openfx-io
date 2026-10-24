/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2026 The Natron Developers
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * Cryptomatte plugin.
 * Extracts mattes from Cryptomatte layers (written from the public Cryptomatte specification v1.2).
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "ofxsCoords.h"
#include "ofxsImageEffect.h"
#include "ofxsMacros.h"
#include "ofxsMultiPlane.h"
#include "ofxsMultiThread.h"
#ifndef OFX_USE_MULTITHREAD_MUTEX
#include "fast_mutex.h"
#endif
#ifdef OFX_EXTENSIONS_NATRON
#include "ofxNatron.h"
#endif

#include "IOUtility.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

using std::string;
using std::vector;
#ifdef OFX_USE_MULTITHREAD_MUTEX
typedef MultiThread::Mutex Mutex;
typedef MultiThread::AutoMutex AutoMutex;
#else
typedef tthread::fast_mutex Mutex;
typedef MultiThread::AutoMutexT<tthread::fast_mutex> AutoMutex;
#endif

#define kPluginName "Cryptomatte"
#define kPluginGrouping "Keyer"
#define kPluginDescription                                                                                         \
    "Extracts mattes from the Cryptomatte layers of a render (e.g. \"CryptoObject\", \"CryptoMaterial\" and "      \
    "\"CryptoAsset\" in multi-layer EXR files).\n\n"                                                                \
    "Cryptomatte stores, for each pixel, the IDs of the objects it covers and their coverage, so the matte of "    \
    "any object or group of objects can be extracted afterwards, with antialiasing, motion blur and transparency.\n\n" \
    "The Matte List holds the names of the objects to extract, separated by commas. Names may contain the "       \
    "wildcards * and ? (matched against the names of the manifest), and objects without a known name are given "  \
    "as their 32-bit ID in hexadecimal between angle brackets, e.g. <0b31652c>.\n\n"                               \
    "Pick objects in the viewer with \"Picker Add\" / \"Picker Remove\", or choose names from the manifest.\n\n"   \
    "The manifest (the list of names) is in the metadata of the file: set \"Metadata File\" to the EXR file "      \
    "(or to its JSON sidecar manifest). Without it, names can still be typed, and picked objects are given by ID.\n\n" \
    "See the Cryptomatte specification: https://github.com/Psyop/Cryptomatte"

#define kPluginIdentifier "fr.inria.openfx.Cryptomatte"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe

#define kParamLayerChoice "cryptoLayerChoice"
#define kParamLayerChoiceLabel "Layer"
#define kParamLayerChoiceHint "Cryptomatte layer of the input to use."

#define kParamLayer "cryptoLayer"
#define kParamLayerLabel "Layer Name"
#define kParamLayerHint "Name of the Cryptomatte layer (e.g. CryptoObject): its ranks are the input layers <name>00, <name>01..."

#define kParamMetadataFile "metadataFile"
#define kParamMetadataFileLabel "Metadata File"
#define kParamMetadataFileHint "EXR file whose header holds the Cryptomatte manifest (usually the file being read), or a JSON manifest. Gives the names of the objects."

#define kParamManifestInfo "manifestInfo"
#define kParamManifestInfoLabel "Manifest"
#define kParamManifestInfoHint "What was found in the metadata file."

#define kParamNameChoice "nameChoice"
#define kParamNameChoiceLabel "Names"
#define kParamNameChoiceHint "Names in the manifest. Use the buttons to add the selected name to the Matte List or remove it."

#define kParamAddName "addName"
#define kParamAddNameLabel "Add"
#define kParamAddNameHint "Add the selected name to the Matte List."

#define kParamRemoveName "removeName"
#define kParamRemoveNameLabel "Remove"
#define kParamRemoveNameHint "Remove the selected name from the Matte List."

#define kParamPickerAdd "pickerAdd"
#define kParamPickerAddLabel "Picker Add"
#define kParamPickerAddHint "When checked, clicking in the viewer adds the object under the pointer to the Matte List."

#define kParamPickerRemove "pickerRemove"
#define kParamPickerRemoveLabel "Picker Remove"
#define kParamPickerRemoveHint "When checked, clicking in the viewer removes the object under the pointer from the Matte List."

#define kParamMatteList "matteList"
#define kParamMatteListLabel "Matte List"
#define kParamMatteListHint "Objects to extract, separated by commas: names (with optional wildcards * and ?, quote names containing commas), or IDs as <hex>."

#define kParamClear "clear"
#define kParamClearLabel "Clear"
#define kParamClearHint "Empty the Matte List."

#define kParamOutput "output"
#define kParamOutputLabel "Output"
#define kParamOutputHint "What the node outputs."
#define kParamOutputOptionMatte "Matte in Alpha", "RGB from the input, the matte in alpha (not premultiplied).", "matte"
#define kParamOutputOptionMatteOnly "Matte Only", "The matte in all channels.", "matteOnly"
#define kParamOutputOptionPremult "Premultiplied", "The input multiplied by the matte.", "premult"
#define kParamOutputOptionPreview "Preview", "A color per object (selected objects bright, others dark), the matte in alpha.", "preview"
enum OutputEnum
{
    eOutputMatte = 0,
    eOutputMatteOnly,
    eOutputPremult,
    eOutputPreview,
};

static bool gHostIsNatron = false;

// MurmurHash3_x86_32 (public domain algorithm by Austin Appleby), seed 0: the hash of the specification.
static uint32_t
murmur3_32(const string& key)
{
    const uint8_t* data = (const uint8_t*)key.data();
    const int len = (int)key.size();
    const int nblocks = len / 4;
    uint32_t h1 = 0;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;

    for (int i = 0; i < nblocks; ++i) {
        uint32_t k1;
        std::memcpy(&k1, data + i * 4, 4);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h1 ^= k1;
        h1 = (h1 << 13) | (h1 >> 19);
        h1 = h1 * 5 + 0xe6546b64;
    }
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3:
        k1 ^= tail[2] << 16;
        // fallthrough
    case 2:
        k1 ^= tail[1] << 8;
        // fallthrough
    case 1:
        k1 ^= tail[0];
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h1 ^= k1;
    }
    h1 ^= len;
    h1 ^= h1 >> 16;
    h1 *= 0x85ebca6b;
    h1 ^= h1 >> 13;
    h1 *= 0xc2b2ae35;
    h1 ^= h1 >> 16;

    return h1;
}

// The ID of a name, as the bits of the float stored in the image ("uint32_to_float32" conversion:
// the exponent is changed to avoid denormals, infinities and NaNs).
static uint32_t
nameToId(const string& name)
{
    uint32_t h = murmur3_32(name);
    uint32_t exponent = (h >> 23) & 255;

    if ( (exponent == 0) || (exponent == 255) ) {
        h ^= 1u << 23;
    }

    return h;
}

static inline uint32_t
floatBits(float f)
{
    uint32_t b;

    std::memcpy(&b, &f, 4);

    return b;
}

static string
idToHex(uint32_t id)
{
    char buf[16];

    std::snprintf(buf, sizeof(buf), "<%08x>", id);

    return buf;
}

// "<0b31652c>" (hex bits) or "<3.4e-32>" (the float value) -> id
static bool
parseIdEntry(const string& entry,
             uint32_t* id)
{
    if ( (entry.size() < 3) || (entry[0] != '<') || (entry[entry.size() - 1] != '>') ) {
        return false;
    }
    string s = entry.substr(1, entry.size() - 2);
    if ( (s.size() == 8) && (s.find_first_not_of("0123456789abcdefABCDEF") == string::npos) ) {
        *id = (uint32_t)std::strtoul(s.c_str(), NULL, 16);

        return true;
    }
    char* end = NULL;
    float f = std::strtof(s.c_str(), &end);
    if ( end && (*end == 0) && !s.empty() ) {
        *id = floatBits(f);

        return true;
    }

    return false;
}

// glob matching with * and ?
static bool
wildcardMatch(const char* pattern,
              const char* str)
{
    if (*pattern == 0) {
        return *str == 0;
    }
    if (*pattern == '*') {
        return wildcardMatch(pattern + 1, str) || ( *str && wildcardMatch(pattern, str + 1) );
    }
    if ( *str && ( (*pattern == '?') || (*pattern == *str) ) ) {
        return wildcardMatch(pattern + 1, str + 1);
    }

    return false;
}

static string
trim(const string& s)
{
    size_t b = s.find_first_not_of(" \t\n\r");

    if (b == string::npos) {
        return string();
    }
    size_t e = s.find_last_not_of(" \t\n\r");

    return s.substr(b, e - b + 1);
}

// Splits the matte list on commas; "quoted names" may contain commas (\" and \\ are escapes).
static vector<string>
parseMatteList(const string& list)
{
    vector<string> ret;
    string cur;
    bool quoted = false;
    bool wasQuoted = false;

    for (size_t i = 0; i < list.size(); ++i) {
        char c = list[i];
        if (quoted) {
            if ( (c == '\\') && (i + 1 < list.size()) ) {
                cur += list[++i];
            } else if (c == '"') {
                quoted = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            quoted = true;
            wasQuoted = true;
        } else if (c == ',') {
            string e = wasQuoted ? cur : trim(cur);
            if ( !e.empty() ) {
                ret.push_back(e);
            }
            cur.clear();
            wasQuoted = false;
        } else if ( !wasQuoted || ( (c != ' ') && (c != '\t') ) ) {
            cur += c;
        }
    }
    string e = wasQuoted ? cur : trim(cur);
    if ( !e.empty() ) {
        ret.push_back(e);
    }

    return ret;
}

static string
formatMatteListEntry(const string& name)
{
    if ( (name.find_first_of(",\"\\") == string::npos) && (trim(name) == name) ) {
        return name;
    }
    string ret = "\"";
    for (size_t i = 0; i < name.size(); ++i) {
        if ( (name[i] == '"') || (name[i] == '\\') ) {
            ret += '\\';
        }
        ret += name[i];
    }

    return ret + "\"";
}

static string
joinMatteList(const vector<string>& entries)
{
    string ret;

    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) {
            ret += ", ";
        }
        ret += formatMatteListEntry(entries[i]);
    }

    return ret;
}

// Minimal parser for the manifest: a flat JSON object of string values, {"name": "hex", ...}.
static bool
parseManifest(const string& json,
              std::map<string, uint32_t>* names)
{
    size_t i = 0;
    const size_t n = json.size();
    auto skipSpace = [&]() {
        while ( i < n && std::strchr(" \t\r\n", json[i]) ) {
            ++i;
        }
    };
    auto parseString = [&](string* out) -> bool {
        skipSpace();
        if ( (i >= n) || (json[i] != '"') ) {
            return false;
        }
        ++i;
        out->clear();
        while (i < n && json[i] != '"') {
            char c = json[i++];
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (i >= n) {
                return false;
            }
            c = json[i++];
            switch (c) {
            case 'n': *out += '\n'; break;
            case 't': *out += '\t'; break;
            case 'r': *out += '\r'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                if (i + 4 > n) {
                    return false;
                }
                unsigned cp = (unsigned)std::strtoul(json.substr(i, 4).c_str(), NULL, 16);
                i += 4;
                // UTF-8 encode (surrogate pairs are kept as two code points: rare in names)
                if (cp < 0x80) {
                    *out += (char)cp;
                } else if (cp < 0x800) {
                    *out += (char)(0xC0 | (cp >> 6));
                    *out += (char)(0x80 | (cp & 0x3F));
                } else {
                    *out += (char)(0xE0 | (cp >> 12));
                    *out += (char)(0x80 | ( (cp >> 6) & 0x3F ));
                    *out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: *out += c; break; // \" \\ \/
            }
        }
        if (i >= n) {
            return false;
        }
        ++i; // closing quote

        return true;
    };

    skipSpace();
    if ( (i >= n) || (json[i] != '{') ) {
        return false;
    }
    ++i;
    skipSpace();
    if ( (i < n) && (json[i] == '}') ) {
        return true;
    }
    while (i < n) {
        string name, hex;
        if ( !parseString(&name) ) {
            return false;
        }
        skipSpace();
        if ( (i >= n) || (json[i] != ':') ) {
            return false;
        }
        ++i;
        if ( !parseString(&hex) ) {
            return false;
        }
        (*names)[name] = (uint32_t)std::strtoul(hex.c_str(), NULL, 16);
        skipSpace();
        if ( (i < n) && (json[i] == ',') ) {
            ++i;
            continue;
        }
        if ( (i < n) && (json[i] == '}') ) {
            return true;
        }

        return false;
    }

    return false;
} // parseManifest

static bool
readTextFile(const string& path,
             string* contents)
{
    FILE* f = std::fopen(path.c_str(), "rb");

    if (!f) {
        return false;
    }
    char buf[65536];
    size_t r;
    contents->clear();
    while ( ( r = std::fread(buf, 1, sizeof(buf), f) ) > 0 ) {
        contents->append(buf, r);
    }
    std::fclose(f);

    return true;
}

// A color for an ID, for the preview (spread over the hue circle by the ID bits).
static void
idColor(uint32_t id,
        float rgb[3])
{
    const uint32_t h = murmur3_32( string( (const char*)&id, 4 ) );

    rgb[0] = 0.2f + 0.8f * ( (h & 0xFF) / 255.f );
    rgb[1] = 0.2f + 0.8f * ( ( (h >> 8) & 0xFF ) / 255.f );
    rgb[2] = 0.2f + 0.8f * ( ( (h >> 16) & 0xFF ) / 255.f );
}

class CryptomattePlugin
    : public ImageEffect
{
public:
    CryptomattePlugin(OfxImageEffectHandle handle);

    // Adds (or removes) the object at the canonical position pos to the matte list. Called by the interact.
    bool pick(double time, const OfxPointD& renderScale, const OfxPointD& pos, bool add);

    bool isPicking() const
    {
        return _pickerAdd->getValue() || _pickerRemove->getValue();
    }

    bool isPickerAdd() const { return _pickerAdd->getValue(); }

private:
    virtual void render(const RenderArguments& args) OVERRIDE FINAL;
    virtual bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE FINAL;
    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;
    virtual void changedClip(const InstanceChangedArgs& args, const string& clipName) OVERRIDE FINAL;
    virtual void getClipPreferences(ClipPreferencesSetter& clipPreferences) OVERRIDE FINAL;
#ifdef OFX_EXTENSIONS_NUKE
    virtual OfxStatus getClipComponents(const ClipComponentsArguments& args, ClipComponentsSetter& clipComponents) OVERRIDE FINAL;
#endif

    // the input layers <layer>00, <layer>01... (OFX plane strings), in rank order
    vector<string> getRankPlanes(const string& layer) const;
    // the names of the cryptomatte layers present in the input
    vector<string> getInputLayers() const;
    string getLayer() const;
    void refreshLayerChoice();
    void loadManifest();
    void refreshNameChoice();
    // the IDs selected by the matte list (sorted)
    vector<uint32_t> getSelectedIds(double time);
    void setMatteListEntry(const string& name, uint32_t id, bool add);

    Clip* _dstClip;
    Clip* _srcClip;
    ChoiceParam* _layerChoice;
    StringParam* _layer;
    StringParam* _metadataFile;
    StringParam* _manifestInfo;
    ChoiceParam* _nameChoice;
    BooleanParam* _pickerAdd;
    BooleanParam* _pickerRemove;
    StringParam* _matteList;
    ChoiceParam* _output;

    Mutex _manifestMutex;
    std::map<string, uint32_t> _manifest; // name -> id, for the current layer
    string _manifestLayer, _manifestFile;  // what _manifest was loaded from
};

CryptomattePlugin::CryptomattePlugin(OfxImageEffectHandle handle)
    : ImageEffect(handle)
    , _dstClip(NULL)
    , _srcClip(NULL)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _layerChoice = fetchChoiceParam(kParamLayerChoice);
    _layer = fetchStringParam(kParamLayer);
    _metadataFile = fetchStringParam(kParamMetadataFile);
    _manifestInfo = fetchStringParam(kParamManifestInfo);
    _nameChoice = fetchChoiceParam(kParamNameChoice);
    _pickerAdd = fetchBooleanParam(kParamPickerAdd);
    _pickerRemove = fetchBooleanParam(kParamPickerRemove);
    _matteList = fetchStringParam(kParamMatteList);
    _output = fetchChoiceParam(kParamOutput);
    assert(_layerChoice && _layer && _metadataFile && _manifestInfo && _nameChoice && _pickerAdd && _pickerRemove && _matteList && _output);
    if (!gHostIsNatron) {
        // only Natron can change the entries of a choice param
        _layerChoice->setIsSecretAndDisabled(true);
        _nameChoice->setIsSecretAndDisabled(true);
    }
    loadManifest();
    refreshLayerChoice();
}

vector<string>
CryptomattePlugin::getInputLayers() const
{
    vector<string> ret;

#ifdef OFX_EXTENSIONS_NUKE
    if ( !_srcClip || !_srcClip->isConnected() ) {
        return ret;
    }
    vector<string> planes;
    _srcClip->getPlanesPresent(&planes);
    for (size_t i = 0; i < planes.size(); ++i) {
        MultiPlane::ImagePlaneDesc desc = MultiPlane::ImagePlaneDesc::mapOFXPlaneStringToPlane(planes[i]);
        const string& label = desc.getPlaneLabel();
        if ( (label.size() > 2) && (label.compare(label.size() - 2, 2, "00") == 0) && (desc.getNumComponents() == 4) ) {
            ret.push_back( label.substr(0, label.size() - 2) );
        }
    }
#endif

    return ret;
}

vector<string>
CryptomattePlugin::getRankPlanes(const string& layer) const
{
    vector<string> ret;

#ifdef OFX_EXTENSIONS_NUKE
    if ( !_srcClip || !_srcClip->isConnected() || layer.empty() ) {
        return ret;
    }
    vector<string> planes;
    _srcClip->getPlanesPresent(&planes);
    std::map<string, string> byLabel;
    for (size_t i = 0; i < planes.size(); ++i) {
        MultiPlane::ImagePlaneDesc desc = MultiPlane::ImagePlaneDesc::mapOFXPlaneStringToPlane(planes[i]);
        if (desc.getNumComponents() == 4) {
            byLabel[desc.getPlaneLabel()] = planes[i];
        }
    }
    for (int r = 0; r < 100; ++r) {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%02d", r);
        std::map<string, string>::const_iterator found = byLabel.find(layer + suffix);
        if ( found == byLabel.end() ) {
            break;
        }
        ret.push_back(found->second);
    }
#endif

    return ret;
}

string
CryptomattePlugin::getLayer() const
{
    string layer;

    _layer->getValue(layer);
    if ( layer.empty() ) {
        vector<string> layers = getInputLayers();
        if ( !layers.empty() ) {
            layer = layers[0];
        }
    }

    return layer;
}

void
CryptomattePlugin::refreshLayerChoice()
{
    if (!gHostIsNatron) {
        return;
    }
    vector<string> layers = getInputLayers();
    string layer = getLayer();
    if ( !layer.empty() && ( std::find(layers.begin(), layers.end(), layer) == layers.end() ) ) {
        layers.push_back(layer); // keep the saved layer in the menu even if the input does not have it (yet)
    }
    _layerChoice->resetOptions();
    for (size_t i = 0; i < layers.size(); ++i) {
        _layerChoice->appendOption(layers[i]);
        if (layers[i] == layer) {
            _layerChoice->setValue( (int)i );
        }
    }
}

void
CryptomattePlugin::loadManifest()
{
    string file;

    _metadataFile->getValue(file);
    const string layer = getLayer();
    std::map<string, uint32_t> manifest;
    string info;
    if ( file.empty() ) {
        info = "No metadata file: names are not known.";
    } else if ( (file.size() > 5) && (file.compare(file.size() - 5, 5, ".json") == 0) ) {
        string json;
        if ( !readTextFile(file, &json) ) {
            info = "Cannot read " + file;
        } else if ( !parseManifest(json, &manifest) ) {
            info = "Invalid JSON manifest in " + file;
        }
    } else {
        auto in = OIIO::ImageInput::open(file);
        if (!in) {
            info = "Cannot read " + file + ": " + OIIO::geterror();
        } else {
            const OIIO::ImageSpec& spec = in->spec();
            string key, layersFound;
            for (const OIIO::ParamValue& p : spec.extra_attribs) {
                const string name = p.name().string();
                if ( (name.compare(0, 12, "cryptomatte/") == 0) && (name.size() > 17) && (name.compare(name.size() - 5, 5, "/name") == 0) ) {
                    const string value = spec.get_string_attribute(name);
                    layersFound += (layersFound.empty() ? "" : ", ") + value;
                    if (value == layer) {
                        key = name.substr(12, name.size() - 17);
                    }
                }
            }
            if ( key.empty() ) {
                info = layersFound.empty() ? "No Cryptomatte metadata in " + file
                       : "No metadata for layer " + layer + " (found: " + layersFound + ")";
            } else {
                const string prefix = "cryptomatte/" + key + "/";
                string json = spec.get_string_attribute(prefix + "manifest");
                const string sidecar = spec.get_string_attribute(prefix + "manif_file");
                if ( json.empty() && !sidecar.empty() ) {
                    // the sidecar manifest is relative to the image
                    size_t slash = file.find_last_of('/');
                    string path = (slash == string::npos) ? sidecar : file.substr(0, slash + 1) + sidecar;
                    if ( !readTextFile(path, &json) ) {
                        info = "Cannot read the manifest " + path;
                    }
                }
                const string hash = spec.get_string_attribute(prefix + "hash", "MurmurHash3_32");
                if (hash != "MurmurHash3_32") {
                    info = "Unsupported hash " + hash;
                } else if ( !json.empty() && !parseManifest(json, &manifest) ) {
                    info = "Invalid manifest in the metadata of " + file;
                }
            }
            in->close();
        }
    }
    if ( info.empty() ) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d names", (int)manifest.size());
        info = buf;
    }
    {
        AutoMutex lock(_manifestMutex);
        _manifest.swap(manifest);
        _manifestLayer = layer;
        _manifestFile = file;
    }
    _manifestInfo->setValue(info);
    refreshNameChoice();
} // CryptomattePlugin::loadManifest

void
CryptomattePlugin::refreshNameChoice()
{
    if (!gHostIsNatron) {
        return;
    }
    _nameChoice->resetOptions();
    AutoMutex lock(_manifestMutex);
    for (std::map<string, uint32_t>::const_iterator it = _manifest.begin(); it != _manifest.end(); ++it) {
        _nameChoice->appendOption(it->first);
    }
}

vector<uint32_t>
CryptomattePlugin::getSelectedIds(double time)
{
    string list;

    _matteList->getValueAtTime(time, list);
    vector<string> entries = parseMatteList(list);
    vector<uint32_t> ids;
    AutoMutex lock(_manifestMutex);
    for (size_t i = 0; i < entries.size(); ++i) {
        uint32_t id;
        if ( parseIdEntry(entries[i], &id) ) {
            ids.push_back(id);
        } else if (entries[i].find_first_of("*?") != string::npos) {
            for (std::map<string, uint32_t>::const_iterator it = _manifest.begin(); it != _manifest.end(); ++it) {
                if ( wildcardMatch( entries[i].c_str(), it->first.c_str() ) ) {
                    ids.push_back(it->second);
                }
            }
        } else {
            std::map<string, uint32_t>::const_iterator found = _manifest.find(entries[i]);
            ids.push_back( found != _manifest.end() ? found->second : nameToId(entries[i]) );
        }
    }
    std::sort( ids.begin(), ids.end() );
    ids.erase( std::unique( ids.begin(), ids.end() ), ids.end() );

    return ids;
}

void
CryptomattePlugin::setMatteListEntry(const string& name,
                                     uint32_t id,
                                     bool add)
{
    string list;

    _matteList->getValue(list);
    vector<string> entries = parseMatteList(list);
    vector<string> kept;
    bool present = false;
    {
        AutoMutex lock(_manifestMutex);
        for (size_t i = 0; i < entries.size(); ++i) {
            uint32_t entryId = 0;
            bool isId = parseIdEntry(entries[i], &entryId);
            if ( !isId && (entries[i].find_first_of("*?") == string::npos) ) {
                std::map<string, uint32_t>::const_iterator found = _manifest.find(entries[i]);
                entryId = ( found != _manifest.end() ) ? found->second : nameToId(entries[i]);
                isId = true;
            }
            if (isId && entryId == id) {
                present = true;
                if (!add) {
                    continue; // remove it
                }
            }
            kept.push_back(entries[i]);
        }
    }
    if (add && !present) {
        kept.push_back(name);
    }
    if ( add && present ) {
        return;
    }
    _matteList->setValue( joinMatteList(kept) );
}

bool
CryptomattePlugin::pick(double time,
                        const OfxPointD& renderScale,
                        const OfxPointD& pos,
                        bool add)
{
#ifdef OFX_EXTENSIONS_NUKE
    vector<string> planes = getRankPlanes( getLayer() );
    if ( planes.empty() ) {
        return false;
    }
    uint32_t bestId = 0;
    float bestCoverage = 0.f;
    for (size_t p = 0; p < planes.size(); ++p) {
        auto_ptr<const Image> img( _srcClip->fetchImagePlane( time, planes[p].c_str() ) );
        if ( !img.get() || (img->getPixelDepth() != eBitDepthFloat) || (img->getPixelComponentCount() != 4) ) {
            continue;
        }
        const double par = img->getPixelAspectRatio();
        const float* pix = (const float*)img->getPixelAddress( (int)std::floor(pos.x * renderScale.x / par), (int)std::floor(pos.y * renderScale.y) );
        if (!pix) {
            // the image may be at full resolution
            pix = (const float*)img->getPixelAddress( (int)std::floor(pos.x / par), (int)std::floor(pos.y) );
        }
        if (!pix) {
            continue;
        }
        for (int k = 0; k < 2; ++k) {
            if (pix[2 * k + 1] > bestCoverage) {
                bestCoverage = pix[2 * k + 1];
                bestId = floatBits(pix[2 * k]);
            }
        }
    }
    if (bestCoverage <= 0.f) {
        return false;
    }
    string name;
    {
        AutoMutex lock(_manifestMutex);
        for (std::map<string, uint32_t>::const_iterator it = _manifest.begin(); it != _manifest.end(); ++it) {
            if (it->second == bestId) {
                name = it->first;
                break;
            }
        }
    }
    if ( name.empty() ) {
        name = idToHex(bestId);
    }
    beginEditBlock(add ? "Cryptomatte Picker Add" : "Cryptomatte Picker Remove");
    setMatteListEntry(name, bestId, add);
    endEditBlock();

    return true;
#else
    (void)time; (void)renderScale; (void)pos; (void)add;

    return false;
#endif
} // CryptomattePlugin::pick

void
CryptomattePlugin::render(const RenderArguments& args)
{
    const double time = args.time;
    auto_ptr<Image> dst( _dstClip->fetchImage(time) );

    if ( !dst.get() ) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    if ( (dst->getPixelDepth() != eBitDepthFloat) || (dst->getPixelComponentCount() != 4) ) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    auto_ptr<const Image> src( ( _srcClip && _srcClip->isConnected() ) ? _srcClip->fetchImage(time) : 0 );
    if ( src.get() && (src->getPixelDepth() != eBitDepthFloat) ) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    const int srcNComps = src.get() ? src->getPixelComponentCount() : 0;

    vector<string> planes;
#ifdef OFX_EXTENSIONS_NUKE
    planes = getRankPlanes( getLayer() );
#endif
    vector<const Image*> ranks;
    vector<auto_ptr<const Image> > rankImages;
    for (size_t p = 0; p < planes.size(); ++p) {
        const Image* img = _srcClip->fetchImagePlane( time, planes[p].c_str() );
        if ( img && ( (img->getPixelDepth() != eBitDepthFloat) || (img->getPixelComponentCount() != 4) ) ) {
            delete img;
            img = NULL;
        }
        rankImages.push_back( auto_ptr<const Image>(img) );
        if (img) {
            ranks.push_back(img);
        }
    }
    if ( planes.empty() && !getLayer().empty() ) {
        setPersistentMessage(Message::eMessageWarning, "", "The input has no Cryptomatte layer " + getLayer() + "00");
    }

    const vector<uint32_t> selected = getSelectedIds(time);
    const OutputEnum output = (OutputEnum)_output->getValueAtTime(time);
    const OfxRectI& win = args.renderWindow;
    for (int y = win.y1; y < win.y2; ++y) {
        if ( abort() ) {
            return;
        }
        float* d = (float*)dst->getPixelAddress(win.x1, y);
        for (int x = win.x1; x < win.x2; ++x, d += 4) {
            float matte = 0.f;
            float preview[3] = {0.f, 0.f, 0.f};
            for (size_t r = 0; r < ranks.size(); ++r) {
                const float* pix = (const float*)ranks[r]->getPixelAddress(x, y);
                if (!pix) {
                    continue;
                }
                for (int k = 0; k < 2; ++k) {
                    const float coverage = pix[2 * k + 1];
                    if (coverage == 0.f) {
                        continue;
                    }
                    const uint32_t id = floatBits(pix[2 * k]);
                    const bool isSelected = std::binary_search(selected.begin(), selected.end(), id);
                    if (isSelected) {
                        matte += coverage;
                    }
                    if (output == eOutputPreview) {
                        float c[3];
                        idColor(id, c);
                        const float gain = isSelected ? 1.f : 0.25f;
                        for (int i = 0; i < 3; ++i) {
                            preview[i] += coverage * c[i] * gain;
                        }
                    }
                }
            }
            matte = (std::max)( 0.f, (std::min)(1.f, matte) );
            const float* s = src.get() ? (const float*)src->getPixelAddress(x, y) : NULL;
            float rgba[4] = {0.f, 0.f, 0.f, 0.f};
            if (s) {
                if (srcNComps >= 3) {
                    rgba[0] = s[0]; rgba[1] = s[1]; rgba[2] = s[2];
                    rgba[3] = (srcNComps == 4) ? s[3] : 1.f;
                } else if (srcNComps == 1) {
                    rgba[3] = s[0];
                }
            }
            switch (output) {
            case eOutputMatte:
                d[0] = rgba[0]; d[1] = rgba[1]; d[2] = rgba[2]; d[3] = matte;
                break;
            case eOutputMatteOnly:
                d[0] = d[1] = d[2] = d[3] = matte;
                break;
            case eOutputPremult:
                d[0] = rgba[0] * matte; d[1] = rgba[1] * matte; d[2] = rgba[2] * matte; d[3] = rgba[3] * matte;
                break;
            case eOutputPreview:
                d[0] = preview[0]; d[1] = preview[1]; d[2] = preview[2]; d[3] = matte;
                break;
            }
        }
    }
} // CryptomattePlugin::render

bool
CryptomattePlugin::isIdentity(const IsIdentityArguments& /*args*/,
                              Clip*& /*identityClip*/,
                              double& /*identityTime*/,
                              int& /*view*/,
                              std::string& /*plane*/)
{
    return false;
}

void
CryptomattePlugin::changedParam(const InstanceChangedArgs& args,
                                const string& paramName)
{
    clearPersistentMessage();
    if ( (paramName == kParamLayerChoice) && (args.reason == eChangeUserEdit) ) {
        int i = _layerChoice->getValue();
        string option;
        _layerChoice->getOption(i, option);
        _layer->setValue(option);
    } else if (paramName == kParamLayer) {
        loadManifest();
        refreshLayerChoice();
    } else if (paramName == kParamMetadataFile) {
        loadManifest();
    } else if ( (paramName == kParamAddName) || (paramName == kParamRemoveName) ) {
        int i = _nameChoice->getValue();
        string name;
        if ( (i >= 0) && ( i < _nameChoice->getNOptions() ) ) {
            _nameChoice->getOption(i, name);
        }
        if ( !name.empty() ) {
            uint32_t id;
            {
                AutoMutex lock(_manifestMutex);
                std::map<string, uint32_t>::const_iterator found = _manifest.find(name);
                id = ( found != _manifest.end() ) ? found->second : nameToId(name);
            }
            setMatteListEntry(name, id, paramName == kParamAddName);
        }
    } else if (paramName == kParamClear) {
        _matteList->setValue("");
    } else if ( (paramName == kParamPickerAdd) && _pickerAdd->getValue() ) {
        _pickerRemove->setValue(false);
    } else if ( (paramName == kParamPickerRemove) && _pickerRemove->getValue() ) {
        _pickerAdd->setValue(false);
    }
}

void
CryptomattePlugin::changedClip(const InstanceChangedArgs& /*args*/,
                               const string& clipName)
{
    if (clipName == kOfxImageEffectSimpleSourceClipName) {
        refreshLayerChoice();
        if ( !_manifestLayer.empty() && (_manifestLayer != getLayer()) ) {
            loadManifest();
        }
    }
}

void
CryptomattePlugin::getClipPreferences(ClipPreferencesSetter& clipPreferences)
{
    clipPreferences.setClipComponents(*_dstClip, ePixelComponentRGBA);
    const OutputEnum output = (OutputEnum)_output->getValue();
    clipPreferences.setOutputPremultiplication( (output == eOutputMatte || output == eOutputPreview) ? eImageUnPreMultiplied : eImagePreMultiplied );
}

#ifdef OFX_EXTENSIONS_NUKE
OfxStatus
CryptomattePlugin::getClipComponents(const ClipComponentsArguments& args,
                                     ClipComponentsSetter& clipComponents)
{
    // the other layers of the input pass through
    clipComponents.setPassThroughClip(_srcClip, args.time, args.view);
    clipComponents.addClipPlane(*_dstClip, kFnOfxImagePlaneColour);
    clipComponents.addClipPlane(*_srcClip, kFnOfxImagePlaneColour);
    vector<string> planes = getRankPlanes( getLayer() );
    for (size_t p = 0; p < planes.size(); ++p) {
        clipComponents.addClipPlane(*_srcClip, planes[p]);
    }

    return kOfxStatOK;
}
#endif

// The viewer pickers: a click adds/removes the object under the pointer.
class CryptomatteInteract
    : public OverlayInteract
{
public:
    CryptomatteInteract(OfxInteractHandle handle,
                        ImageEffect* effect)
        : OverlayInteract(handle)
        , _plugin( dynamic_cast<CryptomattePlugin*>(effect) )
    {
        addParamToSlaveTo( effect->fetchBooleanParam(kParamPickerAdd) );
        addParamToSlaveTo( effect->fetchBooleanParam(kParamPickerRemove) );
    }

private:
    virtual bool penDown(const PenArgs& args) OVERRIDE FINAL
    {
        if ( !_plugin || !_plugin->isPicking() ) {
            return false;
        }

        return _plugin->pick( args.time, args.renderScale, args.penPosition, _plugin->isPickerAdd() );
    }

    CryptomattePlugin* _plugin;
};

class CryptomatteOverlayDescriptor
    : public DefaultEffectOverlayDescriptor<CryptomatteOverlayDescriptor, CryptomatteInteract>
{
};

mDeclarePluginFactory(CryptomattePluginFactory, {}, {});

void
CryptomattePluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
    desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
    desc.setRenderThreadSafety(kRenderThreadSafety);
#ifdef OFX_EXTENSIONS_NUKE
    desc.setIsMultiPlanar(true);
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelPassThroughNonRenderedPlanes);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
    desc.setOverlayInteractDescriptor(new CryptomatteOverlayDescriptor);
}

void
CryptomattePluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                            ContextEnum /*context*/)
{
    gHostIsNatron = getImageEffectHostDescription()->isNatron;

    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamLayerChoice);
        param->setLabel(kParamLayerChoiceLabel);
        param->setHint(kParamLayerChoiceHint);
        param->setIsPersistent(false); // the layer name is saved
        param->setEvaluateOnChange(false);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamLayer);
        param->setLabel(kParamLayerLabel);
        param->setHint(kParamLayerHint);
        param->setDefault("");
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamMetadataFile);
        param->setLabel(kParamMetadataFileLabel);
        param->setHint(kParamMetadataFileHint);
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(true);
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamManifestInfo);
        param->setLabel(kParamManifestInfoLabel);
        param->setHint(kParamManifestInfoHint);
        param->setStringType(eStringTypeLabel);
        param->setIsPersistent(false);
        param->setEvaluateOnChange(false);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamNameChoice);
        param->setLabel(kParamNameChoiceLabel);
        param->setHint(kParamNameChoiceHint);
        param->setIsPersistent(false);
        param->setEvaluateOnChange(false);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamAddName);
        param->setLabel(kParamAddNameLabel);
        param->setHint(kParamAddNameHint);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamRemoveName);
        param->setLabel(kParamRemoveNameLabel);
        param->setHint(kParamRemoveNameHint);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamPickerAdd);
        param->setLabel(kParamPickerAddLabel);
        param->setHint(kParamPickerAddHint);
        param->setDefault(false);
        param->setEvaluateOnChange(false);
        param->setIsPersistent(false);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamPickerRemove);
        param->setLabel(kParamPickerRemoveLabel);
        param->setHint(kParamPickerRemoveHint);
        param->setDefault(false);
        param->setEvaluateOnChange(false);
        param->setIsPersistent(false);
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamMatteList);
        param->setLabel(kParamMatteListLabel);
        param->setHint(kParamMatteListHint);
        param->setDefault("");
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamClear);
        param->setLabel(kParamClearLabel);
        param->setHint(kParamClearHint);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutput);
        param->setLabel(kParamOutputLabel);
        param->setHint(kParamOutputHint);
        assert(param->getNOptions() == eOutputMatte);
        param->appendOption(kParamOutputOptionMatte);
        assert(param->getNOptions() == eOutputMatteOnly);
        param->appendOption(kParamOutputOptionMatteOnly);
        assert(param->getNOptions() == eOutputPremult);
        param->appendOption(kParamOutputOptionPremult);
        assert(param->getNOptions() == eOutputPreview);
        param->appendOption(kParamOutputOptionPreview);
        param->setDefault( (int)eOutputMatte );
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }
} // CryptomattePluginFactory::describeInContext

ImageEffect*
CryptomattePluginFactory::createInstance(OfxImageEffectHandle handle,
                                         ContextEnum /*context*/)
{
    return new CryptomattePlugin(handle);
}

static CryptomattePluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
