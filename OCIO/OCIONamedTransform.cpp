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
 * OCIONamedTransform plugin.
 * Apply a named transform of the OpenColorIO (v2) config.
 */

#ifdef OFX_IO_USING_OCIO

#include <memory>
#ifdef DEBUG
#include <cstdio> // printf
#endif

#include "GenericOCIO.h"

#include "ofxsCoords.h"
#include <ofxNatron.h>
#include <ofxsCopier.h>
#include <ofxsMacros.h>
#include <ofxsProcessing.H>

#include "IOUtility.h"
#include "OCIOPluginBase.h"

using namespace OFX;
using namespace IO;

OFXS_NAMESPACE_ANONYMOUS_ENTER

using std::string;

#define kPluginName "OCIONamedTransformOFX"
#define kPluginGrouping "Color/OCIO"
#define kPluginDescription                                                                \
    "OpenColorIO NamedTransform\n\n"                                                      \
    "Applies a named transform of the OpenColorIO configuration. Named transforms "       \
    "are transforms that are not color spaces, such as the camera log curves or the "     \
    "display encoding curves of the ACES configs (e.g. \"ARRI LogC4 - Curve\"): they "   \
    "only change the encoding, not the primaries.\n\n"                                    \
    "The Inverse direction applies the transform backwards (e.g. log to linear).\n\n"     \
    "Named transforms need an OpenColorIO v2 configuration that defines them."

#define kPluginIdentifier "fr.inria.openfx.OCIONamedTransform"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kParamTransform "transform"
#define kParamTransformLabel "Transform"
#define kParamTransformHint "Name of the named transform to apply (see the menu for the ones defined in the OpenColorIO config)."

#define kParamTransformChoice "transformIndex"
#define kParamTransformChoiceLabel "Transform"
#define kParamTransformChoiceHint "Named transform to apply, among the ones defined in the OpenColorIO config."

#define kParamDirection "direction"
#define kParamDirectionLabel "Direction"
#define kParamDirectionHint "Transform direction."
#define kParamDirectionOptionForward "Forward", "", "forward"
#define kParamDirectionOptionInverse "Inverse", "", "inverse"

namespace OCIO = OCIO_NAMESPACE;

static bool gHostIsNatron = false;

// ChoiceParamType may be ChoiceParamDescriptor or ChoiceParam
template <typename ChoiceParamType>
static void
buildTransformChoiceMenu(OCIO::ConstConfigRcPtr config,
                         ChoiceParamType* choice)
{
    choice->resetOptions();
    if (!config) {
        return;
    }
    for (int i = 0; i < config->getNumNamedTransforms(); ++i) {
        choice->appendOption(config->getNamedTransformNameByIndex(i));
    }
}

class OCIONamedTransformPlugin
    : public OCIOPluginBase {
public:
    OCIONamedTransformPlugin(OfxImageEffectHandle handle);

    virtual ~OCIONamedTransformPlugin();

private:
    /* Override the render */
    virtual void render(const RenderArguments& args) OVERRIDE FINAL;

    /* override is identity */
    virtual bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;

    /* override changed clip */
    virtual void changedClip(const InstanceChangedArgs& args, const string& clipName) OVERRIDE FINAL;

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    virtual void* contextAttached(bool createContextData) OVERRIDE FINAL;
    virtual void contextDetached(void* contextData) OVERRIDE FINAL;

    void renderGPU(const RenderArguments& args);
#endif

    OCIO::ConstProcessorRcPtr getProcessor(OfxTime time);

    // sets the menu entry from the transform name (the name is what is saved and rendered)
    void refreshTransformChoice(double time);

    void setupAndCopy(PixelProcessorFilterBase& processor,
                      double time,
                      const OfxRectI& renderWindow,
                      const OfxPointD& renderScale,
                      const void* srcPixelData,
                      const OfxRectI& srcBounds,
                      PixelComponentEnum srcPixelComponents,
                      int srcPixelComponentCount,
                      BitDepthEnum srcPixelDepth,
                      int srcRowBytes,
                      void* dstPixelData,
                      const OfxRectI& dstBounds,
                      PixelComponentEnum dstPixelComponents,
                      int dstPixelComponentCount,
                      BitDepthEnum dstPixelDepth,
                      int dstRowBytes);

    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI& renderWindow,
                       const OfxPointD& renderScale,
                       const void* srcPixelData,
                       const OfxRectI& srcBounds,
                       PixelComponentEnum srcPixelComponents,
                       int srcPixelComponentCount,
                       BitDepthEnum srcPixelDepth,
                       int srcRowBytes,
                       void* dstPixelData,
                       const OfxRectI& dstBounds,
                       PixelComponentEnum dstPixelComponents,
                       int dstPixelComponentCount,
                       BitDepthEnum dstBitDepth,
                       int dstRowBytes);

    // do not need to delete these, the ImageEffect is managing them for us
    Clip* _dstClip;
    Clip* _srcClip;
    Clip* _maskClip;
    StringParam* _transform;
    ChoiceParam* _transformChoice;
    ChoiceParam* _direction;
    DoubleParam* _mix;
    BooleanParam* _maskApply;
    BooleanParam* _maskInvert;

    auto_ptr<GenericOCIO> _ocio;

    GenericOCIO::Mutex _procMutex;
    OCIO::ConstProcessorRcPtr _proc;
    OCIO::ConstConfigRcPtr _procConfig;
    string _procTransform;
    string _procContext;
    int _procDirection;

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    OCIOOpenGLContextData* _openGLContextData; // (OpenGL-only) - the single openGL context, in case the host does not support kNatronOfxImageEffectPropOpenGLContextData
#endif
};

OCIONamedTransformPlugin::OCIONamedTransformPlugin(OfxImageEffectHandle handle)
    : OCIOPluginBase(handle)
    , _dstClip(NULL)
    , _srcClip(NULL)
    , _maskClip(NULL)
    , _transform(NULL)
    , _transformChoice(NULL)
    , _direction(NULL)
    , _mix(NULL)
    , _maskApply(NULL)
    , _maskInvert(NULL)
    , _ocio(new GenericOCIO(this))
    , _procDirection(-1)
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    , _openGLContextData(NULL)
#endif
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    assert(_dstClip && (!_dstClip->isConnected() || _dstClip->getPixelComponents() == ePixelComponentRGBA || _dstClip->getPixelComponents() == ePixelComponentRGB));
    _srcClip = getContext() == eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert((!_srcClip && getContext() == eContextGenerator) || (_srcClip && (!_srcClip->isConnected() || _srcClip->getPixelComponents() == ePixelComponentRGBA || _srcClip->getPixelComponents() == ePixelComponentRGB)));
    _maskClip = fetchClip(getContext() == eContextPaint ? "Brush" : "Mask");
    assert(!_maskClip || !_maskClip->isConnected() || _maskClip->getPixelComponents() == ePixelComponentAlpha);
    _transform = fetchStringParam(kParamTransform);
    _transformChoice = fetchChoiceParam(kParamTransformChoice);
    _direction = fetchChoiceParam(kParamDirection);
    assert(_transform && _transformChoice && _direction);
    _mix = fetchDoubleParam(kParamMix);
    _maskApply = paramExists(kParamMaskApply) ? fetchBooleanParam(kParamMaskApply) : 0;
    _maskInvert = fetchBooleanParam(kParamMaskInvert);
    assert(_mix && _maskInvert);

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    setSupportsOpenGLAndTileInfo();
#endif

    AutoSetAndRestoreThreadLocale locale;
    OCIO::ConstConfigRcPtr config = _ocio->getConfig();
    if (!config || !gHostIsNatron) {
        // only Natron supports changing the entries of a choice param: other hosts use the name
        _transformChoice->setIsSecretAndDisabled(true);
    } else {
        if (!_ocio->configIsDefault()) {
            buildTransformChoiceMenu(config, _transformChoice);
        }
        refreshTransformChoice(0.);
    }
}

OCIONamedTransformPlugin::~OCIONamedTransformPlugin()
{
}

void
OCIONamedTransformPlugin::refreshTransformChoice(double time)
{
    OCIO::ConstConfigRcPtr config = _ocio->getConfig();
    if (!config || !gHostIsNatron) {
        return;
    }
    string name;
    _transform->getValueAtTime(time, name);
    int index = config->getIndexForNamedTransform( name.c_str() );
    if ( (index >= 0) && (index != _transformChoice->getValueAtTime(time)) ) {
        _transformChoice->setValue(index);
    }
}

/* set up and run a copy processor */
void
OCIONamedTransformPlugin::setupAndCopy(PixelProcessorFilterBase& processor,
                                       double time,
                                       const OfxRectI& renderWindow,
                                       const OfxPointD& renderScale,
                                       const void* srcPixelData,
                                       const OfxRectI& srcBounds,
                                       PixelComponentEnum srcPixelComponents,
                                       int srcPixelComponentCount,
                                       BitDepthEnum srcPixelDepth,
                                       int srcRowBytes,
                                       void* dstPixelData,
                                       const OfxRectI& dstBounds,
                                       PixelComponentEnum dstPixelComponents,
                                       int dstPixelComponentCount,
                                       BitDepthEnum dstPixelDepth,
                                       int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // make sure bit depths are sane
    if ((srcPixelDepth != dstPixelDepth) || (srcPixelComponents != dstPixelComponents)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    auto_ptr<const Image> orig((_srcClip && _srcClip->isConnected()) ? _srcClip->fetchImage(time) : 0);

    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(time)) && _maskClip && _maskClip->isConnected());
    auto_ptr<const Image> mask(doMasking ? _maskClip->fetchImage(time) : 0);
    if (doMasking) {
        bool maskInvert = _maskInvert->getValueAtTime(time);
        processor.doMasking(true);
        processor.setMaskImg(mask.get(), maskInvert);
    }

    if (!orig.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    processor.setOrigImg(orig.get());
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstPixelDepth, dstRowBytes);
    processor.setSrcImg(srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, 0);
    processor.setRenderWindow(renderWindow, renderScale);

    bool premult;
    int premultChannel;
    getPremultAndPremultChannelAtTime(time, premult, premultChannel);
    double mix = _mix->getValueAtTime(time);
    processor.setPremultMaskMix(premult, premultChannel, mix);

    processor.process();
}

void
OCIONamedTransformPlugin::copyPixelData(bool unpremult,
                                        bool premult,
                                        bool maskmix,
                                        double time,
                                        const OfxRectI& renderWindow,
                                        const OfxPointD& renderScale,
                                        const void* srcPixelData,
                                        const OfxRectI& srcBounds,
                                        PixelComponentEnum srcPixelComponents,
                                        int srcPixelComponentCount,
                                        BitDepthEnum srcPixelDepth,
                                        int srcRowBytes,
                                        void* dstPixelData,
                                        const OfxRectI& dstBounds,
                                        PixelComponentEnum dstPixelComponents,
                                        int dstPixelComponentCount,
                                        BitDepthEnum dstBitDepth,
                                        int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);
    if ((dstBitDepth != eBitDepthFloat) || ((dstPixelComponents != ePixelComponentRGBA) && (dstPixelComponents != ePixelComponentRGB))) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    if (!unpremult && !premult && !maskmix) {
        copyPixels(*this, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
    } else if (unpremult && !premult && !maskmix) {
        if (dstPixelComponents == ePixelComponentRGBA) {
            PixelCopierUnPremult<float, 4, 1, float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else {
            PixelCopierUnPremult<float, 3, 1, float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        }
    } else if (!unpremult && !premult && maskmix) {
        if (dstPixelComponents == ePixelComponentRGBA) {
            PixelCopierMaskMix<float, 4, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else {
            PixelCopierMaskMix<float, 3, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        }
    } else if (!unpremult && premult && maskmix) {
        if (dstPixelComponents == ePixelComponentRGBA) {
            PixelCopierPremultMaskMix<float, 4, 1, float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else {
            PixelCopierPremultMaskMix<float, 3, 1, float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        }
    } else {
        assert(false); // should never happen
    }
}

OCIO::ConstProcessorRcPtr
OCIONamedTransformPlugin::getProcessor(OfxTime time)
{
    AutoSetAndRestoreThreadLocale locale;
    OCIO::ConstConfigRcPtr config = _ocio->getConfig();
    if (!config) {
        setPersistentMessage(Message::eMessageError, "", "OCIO: no current config");
        throwSuiteStatusException(kOfxStatFailed);

        return OCIO::ConstProcessorRcPtr();
    }

    string name;
    _transform->getValueAtTime(time, name);
    int directioni = _direction->getValueAtTime(time);
    try {
        OCIO::ConstContextRcPtr context = _ocio->getLocalContext(time);
        string contextID = context ? context->getCacheID() : string();
        GenericOCIO::AutoMutex guard(_procMutex);
        if (!_proc || (_procConfig != config) || (_procTransform != name) || (_procDirection != directioni) || (_procContext != contextID)) {
            OCIO::ConstNamedTransformRcPtr namedTransform = config->getNamedTransform( name.c_str() );
            if (!namedTransform) {
                setPersistentMessage(Message::eMessageError, "", "OCIO: the config has no named transform \"" + name + "\"");
                throwSuiteStatusException(kOfxStatFailed);

                return OCIO::ConstProcessorRcPtr();
            }
            OCIO::TransformDirection direction = (directioni == 0) ? OCIO::TRANSFORM_DIR_FORWARD : OCIO::TRANSFORM_DIR_INVERSE;
            _proc = context ? config->getProcessor(context, namedTransform, direction) : config->getProcessor(namedTransform, direction);
            _procConfig = config;
            _procTransform = name;
            _procDirection = directioni;
            _procContext = contextID;
        }

        return _proc;
    } catch (const OCIO::Exception& e) {
        setPersistentMessage(Message::eMessageError, "", e.what());
        throwSuiteStatusException(kOfxStatFailed);

        return OCIO::ConstProcessorRcPtr();
    }
} // getProcessor

#if defined(OFX_SUPPORTS_OPENGLRENDER)

void*
OCIONamedTransformPlugin::contextAttached(bool createContextData)
{
#ifdef DEBUG
    if (getImageEffectHostDescription()->isNatron && !createContextData) {
        std::printf("ERROR: Natron did not ask to create context data\n");
    }
#endif
    if (createContextData) {
        return new OCIOOpenGLContextData;
    } else {
        if (_openGLContextData) {
#ifdef DEBUG
            std::printf("ERROR: contextAttached() called but context already attached\n");
#endif
            contextDetached(NULL);
        }
        _openGLContextData = new OCIOOpenGLContextData;
    }

    return NULL;
}

void
OCIONamedTransformPlugin::contextDetached(void* contextData)
{
    if (contextData) {
        OCIOOpenGLContextData* myData = (OCIOOpenGLContextData*)contextData;
        delete myData;
    } else {
        delete _openGLContextData;
        _openGLContextData = NULL;
    }
}

void
OCIONamedTransformPlugin::renderGPU(const RenderArguments& args)
{
    auto_ptr<Texture> srcImg(_srcClip->loadTexture(args.time));
    if (!srcImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(srcImg, args);

    auto_ptr<Texture> dstImg(_dstClip->loadTexture(args.time));
    if (!dstImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(dstImg, args);

    BitDepthEnum srcBitDepth = srcImg->getPixelDepth();
    PixelComponentEnum srcComponents = srcImg->getPixelComponents();
    BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if ((dstBitDepth != eBitDepthFloat) || (dstBitDepth != srcBitDepth)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    PixelComponentEnum dstComponents = dstImg->getPixelComponents();
    if (((dstComponents != ePixelComponentRGBA) && (dstComponents != ePixelComponentRGB)) || (dstComponents != srcComponents)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    OCIOOpenGLContextData* contextData = NULL;
    if (args.openGLContextData) {
        // host provided kNatronOfxImageEffectPropOpenGLContextData,
        // which was returned by kOfxActionOpenGLContextAttached
        contextData = (OCIOOpenGLContextData*)args.openGLContextData;
    } else {
        if (!_openGLContextData) {
            contextAttached(false);
            assert(_openGLContextData);
        }
        contextData = _openGLContextData;
    }
    if (!contextData) {
        throwSuiteStatusException(kOfxStatFailed);
    }

    OCIO::ConstProcessorRcPtr proc = getProcessor(args.time);
    assert(proc);

    GenericOCIO::applyGL(srcImg.get(), proc, contextData);
} // renderGPU

#endif // defined(OFX_SUPPORTS_OPENGLRENDER)

/* Override the render */
void
OCIONamedTransformPlugin::render(const RenderArguments& args)
{
    if (!_srcClip || !_dstClip) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    if (args.openGLEnabled) {
        renderGPU(args);

        return;
    }
#endif

    auto_ptr<const Image> srcImg(_srcClip->fetchImage(args.time));
    if (!srcImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(srcImg, args);

    BitDepthEnum srcBitDepth = srcImg->getPixelDepth();
    PixelComponentEnum srcComponents = srcImg->getPixelComponents();

    auto_ptr<Image> dstImg(_dstClip->fetchImage(args.time));
    if (!dstImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(dstImg, args);

    BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if ((dstBitDepth != eBitDepthFloat) || (dstBitDepth != srcBitDepth)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    PixelComponentEnum dstComponents = dstImg->getPixelComponents();
    if (((dstComponents != ePixelComponentRGBA) && (dstComponents != ePixelComponentRGB)) || (dstComponents != srcComponents)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    // are we in the image bounds
    OfxRectI dstBounds = dstImg->getBounds();
    if ((args.renderWindow.x1 < dstBounds.x1) || (args.renderWindow.x1 >= dstBounds.x2) || (args.renderWindow.y1 < dstBounds.y1) || (args.renderWindow.y1 >= dstBounds.y2) || (args.renderWindow.x2 <= dstBounds.x1) || (args.renderWindow.x2 > dstBounds.x2) || (args.renderWindow.y2 <= dstBounds.y1) || (args.renderWindow.y2 > dstBounds.y2)) {
        throwSuiteStatusException(kOfxStatErrValue);

        return;
    }

    const void* srcPixelData = NULL;
    OfxRectI bounds;
    PixelComponentEnum pixelComponents;
    BitDepthEnum bitDepth;
    int srcRowBytes;
    getImageData(srcImg.get(), &srcPixelData, &bounds, &pixelComponents, &bitDepth, &srcRowBytes);
    int pixelComponentCount = srcImg->getPixelComponentCount();

    void* dstPixelData = NULL;
    OfxRectI dstImgBounds;
    PixelComponentEnum dstPixelComponents;
    BitDepthEnum dstImgBitDepth;
    int dstRowBytes;
    getImageData(dstImg.get(), &dstPixelData, &dstImgBounds, &dstPixelComponents, &dstImgBitDepth, &dstRowBytes);
    int dstPixelComponentCount = dstImg->getPixelComponentCount();

    // allocate temporary image
    int pixelBytes = pixelComponentCount * getComponentBytes(srcBitDepth);
    int tmpRowBytes = (args.renderWindow.x2 - args.renderWindow.x1) * pixelBytes;
    size_t memSize = (args.renderWindow.y2 - args.renderWindow.y1) * tmpRowBytes;
    ImageMemory mem(memSize, this);
    float* tmpPixelData = (float*)mem.lock();
    bool premult = getPremultValueAtTime(args.time);

    // copy renderWindow to the temporary image
    copyPixelData(premult, false, false, args.time, args.renderWindow, args.renderScale, srcPixelData, bounds, pixelComponents, pixelComponentCount, bitDepth, srcRowBytes, tmpPixelData, args.renderWindow, pixelComponents, pixelComponentCount, bitDepth, tmpRowBytes);

    // apply the named transform
    OCIOProcessor processor(*this);
    processor.setProcessor(getProcessor(args.time));
    processor.setDstImg(tmpPixelData, args.renderWindow, pixelComponents, pixelComponentCount, eBitDepthFloat, tmpRowBytes);
    processor.setRenderWindow(args.renderWindow, args.renderScale);
    processor.process();

    // copy the converted window and apply masking
    copyPixelData(false, premult, true, args.time, args.renderWindow, args.renderScale, tmpPixelData, args.renderWindow, pixelComponents, pixelComponentCount, bitDepth, tmpRowBytes, dstPixelData, dstImgBounds, dstPixelComponents, dstPixelComponentCount, dstImgBitDepth, dstRowBytes);
} // OCIONamedTransformPlugin::render

bool
OCIONamedTransformPlugin::isIdentity(const IsIdentityArguments& args,
                                     Clip*& identityClip,
                                     double& /*identityTime*/
                                     ,
                                     int& /*view*/, std::string& /*plane*/)
{
    string name;
    _transform->getValueAtTime(args.time, name);
    double mix;
    _mix->getValueAtTime(args.time, mix);
    if ( name.empty() || (mix == 0.) ) {
        identityClip = _srcClip;

        return true;
    }

    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    if (doMasking) {
        bool maskInvert;
        _maskInvert->getValueAtTime(args.time, maskInvert);
        if (!maskInvert) {
            OfxRectI maskRoD;
            Coords::toPixelEnclosing(_maskClip->getRegionOfDefinition(args.time), args.renderScale, _maskClip->getPixelAspectRatio(), &maskRoD);
            // effect is identity if the renderWindow doesn't intersect the mask RoD
            if (!Coords::rectIntersection<OfxRectI>(args.renderWindow, maskRoD, 0)) {
                identityClip = _srcClip;

                return true;
            }
        }
    }

    return false;
}

void
OCIONamedTransformPlugin::changedParam(const InstanceChangedArgs& args,
                                       const string& paramName)
{
    // must clear persistent message, or render() is not called by Nuke after an error
    clearPersistentMessage();
    if (paramName == kParamTransformChoice) {
        if (args.reason == eChangeUserEdit) {
            AutoSetAndRestoreThreadLocale locale;
            OCIO::ConstConfigRcPtr config = _ocio->getConfig();
            int index = _transformChoice->getValueAtTime(args.time);
            const char* name = config ? config->getNamedTransformNameByIndex(index) : NULL;
            if (name && *name) {
                _transform->setValue(name);
            }
        }
    } else if (paramName == kParamTransform) {
        refreshTransformChoice(args.time);
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    } else if (paramEffectsOpenGLAndTileSupport(paramName) || paramName == kParamPremult) {
        setSupportsOpenGLAndTileInfoAtTime(args.time);
#endif
    } else {
        _ocio->changedParam(args, paramName);
    }
    // this must be done after handling by GenericOCIO (to make sure the new config is loaded)
    if ( (paramName == kOCIOParamConfigFile) && (args.reason == eChangeUserEdit) && gHostIsNatron ) {
        AutoSetAndRestoreThreadLocale locale;
        buildTransformChoiceMenu(_ocio->getConfig(), _transformChoice);
        refreshTransformChoice(args.time);
    }
} // OCIONamedTransformPlugin::changedParam

void
OCIONamedTransformPlugin::changedClip(const InstanceChangedArgs& args,
                                      const string& clipName)
{
    if ((clipName == kOfxImageEffectSimpleSourceClipName) && _srcClip && (args.reason == eChangeUserEdit)) {
        changedSrcClip(_srcClip);
    }
}

mDeclarePluginFactory(OCIONamedTransformPluginFactory, { ofxsThreadSuiteCheck(); }, {});

/** @brief The basic describe function, passed a plugin descriptor */
void
OCIONamedTransformPluginFactory::describe(ImageEffectDescriptor& desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextPaint);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    desc.setSupportsOpenGLRender(true);
#endif
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
OCIONamedTransformPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                                   ContextEnum context)
{
    // create the mandated source clip
    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);

    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->setSupportsTiles(kSupportsTiles);

    ClipDescriptor* maskClip = (context == eContextPaint) ? desc.defineClip("Brush") : desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setTemporalClipAccess(false);
    if (context != eContextPaint) {
        maskClip->setOptional(true);
    }
    maskClip->setSupportsTiles(kSupportsTiles);
    maskClip->setIsMask(true);

    gHostIsNatron = (getImageEffectHostDescription()->isNatron);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    GenericOCIO::describeInContextConfig(desc, context, page);
    AutoSetAndRestoreThreadLocale locale;
    OCIO::ConstConfigRcPtr config;
    if ( const char* file = std::getenv("OCIO") ) {
        try {
            config = OCIO::Config::CreateFromFile(file);
        } catch (const OCIO::Exception&) {
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamTransformChoice);
        param->setLabel(kParamTransformChoiceLabel);
        param->setHint(kParamTransformChoiceHint);
        if (config) {
            buildTransformChoiceMenu(config, param);
        }
        param->setAnimates(false);
        param->setEvaluateOnChange(false); // evaluate only when the StringParam is changed
        param->setIsPersistent(false); // the name is saved, not the menu index
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamTransform);
        param->setLabel(kParamTransformLabel);
        param->setHint(kParamTransformHint);
        if (config && (config->getNumNamedTransforms() > 0)) {
            param->setDefault( config->getNamedTransformNameByIndex(0) );
        }
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamDirection);
        param->setLabel(kParamDirectionLabel);
        param->setHint(kParamDirectionHint);
        param->appendOption(kParamDirectionOptionForward);
        param->appendOption(kParamDirectionOptionInverse);
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }
    GenericOCIO::describeInContextContext(desc, context, page);
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kOCIOHelpButton);
        param->setLabel(kOCIOHelpButtonLabel);
        param->setHint(kOCIOHelpButtonHint);
        if (page) {
            page->addChild(*param);
        }
    }

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    OCIOPluginBase::defineEnableGPUParam(desc, page);
#endif

    ofxsPremultDescribeParams(desc, page);
    ofxsMaskMixDescribeParams(desc, page);
} // OCIONamedTransformPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
OCIONamedTransformPluginFactory::createInstance(OfxImageEffectHandle handle,
                                                ContextEnum /*context*/)
{
    return new OCIONamedTransformPlugin(handle);
}

static OCIONamedTransformPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

    OFXS_NAMESPACE_ANONYMOUS_EXIT

#endif // OFX_IO_USING_OCIO
