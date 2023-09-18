// Copyright 2020-2023 CesiumGS, Inc. and Contributors

#include "CesiumEncodedFeaturesMetadata.h"
#include "CesiumEncodedMetadataConversions.h"
#include "CesiumFeatureIdSet.h"
#include "CesiumFeaturesMetadataComponent.h"
#include "CesiumLifetime.h"
#include "CesiumMetadataConversions.h"
#include "CesiumModelMetadata.h"
#include "CesiumPrimitiveFeatures.h"
#include "CesiumPrimitiveMetadata.h"
#include "CesiumPropertyArray.h"
#include "CesiumPropertyTable.h"
#include "CesiumPropertyTexture.h"
#include "CesiumRuntime.h"
#include "Containers/Map.h"
#include "PixelFormat.h"
#include "TextureResource.h"
#include <CesiumGltf/FeatureIdTextureView.h>
#include <CesiumGltf/PropertyTextureView.h>
#include <CesiumUtility/Tracing.h>
#include <glm/gtx/integer.hpp>
#include <optional>
#include <unordered_map>

using namespace CesiumTextureUtility;

namespace CesiumEncodedFeaturesMetadata {

FString getNameForFeatureIDSet(
    const FCesiumFeatureIdSet& featureIDSet,
    int32& FeatureIdTextureCounter) {
  FString label = UCesiumFeatureIdSetBlueprintLibrary::GetLabel(featureIDSet);
  if (!label.IsEmpty()) {
    return label;
  }

  ECesiumFeatureIdSetType type =
      UCesiumFeatureIdSetBlueprintLibrary::GetFeatureIDSetType(featureIDSet);

  if (type == ECesiumFeatureIdSetType::Attribute) {
    FCesiumFeatureIdAttribute attribute =
        UCesiumFeatureIdSetBlueprintLibrary::GetAsFeatureIDAttribute(
            featureIDSet);
    ECesiumFeatureIdAttributeStatus status =
        UCesiumFeatureIdAttributeBlueprintLibrary::GetFeatureIDAttributeStatus(
            attribute);
    if (status == ECesiumFeatureIdAttributeStatus::Valid) {
      std::string generatedName =
          "_FEATURE_ID_" + std::to_string(attribute.getAttributeIndex());
      return FString(generatedName.c_str());
    }
  }

  if (type == ECesiumFeatureIdSetType::Texture) {
    std::string generatedName =
        "_FEATURE_ID_TEXTURE_" + std::to_string(FeatureIdTextureCounter);
    FeatureIdTextureCounter++;
    return FString(generatedName.c_str());
  }

  if (type == ECesiumFeatureIdSetType::Implicit) {
    return FString("_IMPLICIT_FEATURE_ID");
  }

  // If for some reason an empty / invalid feature ID set was constructed,
  // return an empty name.
  return FString();
}

namespace {

/**
 * @brief Encodes a feature ID attribute for access in a Unreal Engine Material.
 * The feature IDs are simply sent to the GPU as texture coordinates, so this
 * just handles the variable names necessary for material access.
 *
 * @returns The encoded feature ID attribute, or std::nullopt if the attribute
 * was somehow invalid.
 */
std::optional<EncodedFeatureIdSet>
encodeFeatureIdAttribute(const FCesiumFeatureIdAttribute& attribute) {
  const ECesiumFeatureIdAttributeStatus status =
      UCesiumFeatureIdAttributeBlueprintLibrary::GetFeatureIDAttributeStatus(
          attribute);

  if (status != ECesiumFeatureIdAttributeStatus::Valid) {
    UE_LOG(
        LogCesium,
        Warning,
        TEXT("Can't encode invalid feature ID attribute, skipped."));
    return std::nullopt;
  }

  EncodedFeatureIdSet result;
  result.attribute = attribute.getAttributeIndex();
  return result;
}

std::optional<EncodedFeatureIdSet> encodeFeatureIdTexture(
    const FCesiumFeatureIdTexture& texture,
    TMap<const CesiumGltf::ImageCesium*, TWeakPtr<LoadedTextureResult>>&
        featureIdTextureMap) {
  const ECesiumFeatureIdTextureStatus status =
      UCesiumFeatureIdTextureBlueprintLibrary::GetFeatureIDTextureStatus(
          texture);
  if (status != ECesiumFeatureIdTextureStatus::Valid) {
    UE_LOG(
        LogCesium,
        Warning,
        TEXT("Can't encode invalid feature ID texture, skipped."));
    return std::nullopt;
  }

  const CesiumGltf::FeatureIdTextureView& featureIdTextureView =
      texture.getFeatureIdTextureView();
  const CesiumGltf::ImageCesium* pFeatureIdImage =
      featureIdTextureView.getImage();

  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeFeatureIdTexture)

  EncodedFeatureIdSet result;
  EncodedFeatureIdTexture& encodedFeatureIdTexture = result.texture.emplace();

  encodedFeatureIdTexture.channels = featureIdTextureView.getChannels();
  encodedFeatureIdTexture.textureCoordinateSetIndex =
      featureIdTextureView.getTexCoordSetIndex();

  TWeakPtr<LoadedTextureResult>* pMappedUnrealImageIt =
      featureIdTextureMap.Find(pFeatureIdImage);
  if (pMappedUnrealImageIt) {
    encodedFeatureIdTexture.pTexture = pMappedUnrealImageIt->Pin();
  } else {
    encodedFeatureIdTexture.pTexture = MakeShared<LoadedTextureResult>();
    // TODO: upgrade to new texture creation path
    encodedFeatureIdTexture.pTexture->textureSource = LegacyTextureSource{};
    featureIdTextureMap.Emplace(
        pFeatureIdImage,
        encodedFeatureIdTexture.pTexture);
    encodedFeatureIdTexture.pTexture->pTextureData = createTexturePlatformData(
        pFeatureIdImage->width,
        pFeatureIdImage->height,
        // TODO: currently this is always the case, but doesn't have to be
        EPixelFormat::PF_R8G8B8A8_UINT);

    encodedFeatureIdTexture.pTexture->addressX = TextureAddress::TA_Clamp;
    encodedFeatureIdTexture.pTexture->addressY = TextureAddress::TA_Clamp;
    encodedFeatureIdTexture.pTexture->filter = TextureFilter::TF_Nearest;

    if (!encodedFeatureIdTexture.pTexture->pTextureData) {
      UE_LOG(
          LogCesium,
          Error,
          TEXT(
              "Error encoding a feature ID texture. Most likely could not allocate enough texture memory."));
      return std::nullopt;
    }

    FTexture2DMipMap* pMip = new FTexture2DMipMap();
    encodedFeatureIdTexture.pTexture->pTextureData->Mips.Add(pMip);
    pMip->SizeX = pFeatureIdImage->width;
    pMip->SizeY = pFeatureIdImage->height;
    pMip->BulkData.Lock(LOCK_READ_WRITE);

    void* pTextureData =
        pMip->BulkData.Realloc(pFeatureIdImage->pixelData.size());

    FMemory::Memcpy(
        pTextureData,
        pFeatureIdImage->pixelData.data(),
        pFeatureIdImage->pixelData.size());

    pMip->BulkData.Unlock();
  }

  return result;
}
} // namespace

EncodedPrimitiveFeatures encodePrimitiveFeaturesAnyThreadPart(
    const FCesiumPrimitiveFeaturesDescription& featuresDescription,
    const FCesiumPrimitiveFeatures& features) {
  EncodedPrimitiveFeatures result;

  const TArray<FCesiumFeatureIdSetDescription>& featureIDSetDescriptions =
      featuresDescription.FeatureIdSets;
  result.featureIdSets.Reserve(featureIDSetDescriptions.Num());

  // Not all feature ID sets are necessarily textures, but reserve the max
  // amount just in case.
  TMap<const CesiumGltf::ImageCesium*, TWeakPtr<LoadedTextureResult>>
      featureIdTextureMap;
  featureIdTextureMap.Reserve(featureIDSetDescriptions.Num());

  const TArray<FCesiumFeatureIdSet>& featureIdSets =
      UCesiumPrimitiveFeaturesBlueprintLibrary::GetFeatureIDSets(features);
  int32_t featureIdTextureCounter = 0;

  for (int32 i = 0; i < featureIdSets.Num(); i++) {
    const FCesiumFeatureIdSet& set = featureIdSets[i];
    FString name = getNameForFeatureIDSet(set, featureIdTextureCounter);
    const FCesiumFeatureIdSetDescription* pDescription =
        featureIDSetDescriptions.FindByPredicate(
            [&name](
                const FCesiumFeatureIdSetDescription& existingFeatureIDSet) {
              return existingFeatureIDSet.Name == name;
            });

    if (!pDescription) {
      // The description doesn't need this feature ID set, skip.
      continue;
    }

    std::optional<EncodedFeatureIdSet> encodedSet;
    ECesiumFeatureIdSetType type =
        UCesiumFeatureIdSetBlueprintLibrary::GetFeatureIDSetType(set);

    if (type == ECesiumFeatureIdSetType::Attribute) {
      const FCesiumFeatureIdAttribute& attribute =
          UCesiumFeatureIdSetBlueprintLibrary::GetAsFeatureIDAttribute(set);
      encodedSet = encodeFeatureIdAttribute(attribute);
    } else if (type == ECesiumFeatureIdSetType::Texture) {
      const FCesiumFeatureIdTexture& texture =
          UCesiumFeatureIdSetBlueprintLibrary::GetAsFeatureIDTexture(set);
      encodedSet = encodeFeatureIdTexture(texture, featureIdTextureMap);
    } else if (type == ECesiumFeatureIdSetType::Implicit) {
      encodedSet = EncodedFeatureIdSet();
    }

    if (!encodedSet)
      continue;

    encodedSet->name = name;
    encodedSet->index = i;
    encodedSet->propertyTableName = pDescription->PropertyTableName;
    encodedSet->nullFeatureId =
        UCesiumFeatureIdSetBlueprintLibrary::GetNullFeatureID(set);

    result.featureIdSets.Add(*encodedSet);
  }

  return result;
}

bool encodePrimitiveFeaturesGameThreadPart(
    EncodedPrimitiveFeatures& encodedFeatures) {
  bool success = true;

  // Not all feature ID sets are necessarily textures, but reserve the max
  // amount just in case.
  TArray<const LoadedTextureResult*> uniqueFeatureIdImages;
  uniqueFeatureIdImages.Reserve(encodedFeatures.featureIdSets.Num());

  for (EncodedFeatureIdSet& encodedFeatureIdSet :
       encodedFeatures.featureIdSets) {
    if (!encodedFeatureIdSet.texture) {
      continue;
    }

    auto& encodedFeatureIdTexture = *encodedFeatureIdSet.texture;
    if (uniqueFeatureIdImages.Find(encodedFeatureIdTexture.pTexture.Get()) ==
        INDEX_NONE) {
      success &= loadTextureGameThreadPart(
                     encodedFeatureIdTexture.pTexture.Get()) != nullptr;
      uniqueFeatureIdImages.Emplace(encodedFeatureIdTexture.pTexture.Get());
    }
  }

  return success;
}

void destroyEncodedPrimitiveFeatures(
    EncodedPrimitiveFeatures& encodedFeatures) {
  for (EncodedFeatureIdSet& encodedFeatureIdSet :
       encodedFeatures.featureIdSets) {
    if (!encodedFeatureIdSet.texture) {
      continue;
    }

    auto& encodedFeatureIdTexture = *encodedFeatureIdSet.texture;
    if (encodedFeatureIdTexture.pTexture->pTexture.IsValid()) {
      CesiumLifetime::destroy(encodedFeatureIdTexture.pTexture->pTexture.Get());
      encodedFeatureIdTexture.pTexture->pTexture.Reset();
    }
  }
}

FString getNameForPropertyTable(const FCesiumPropertyTable& PropertyTable) {
  FString propertyTableName =
      UCesiumPropertyTableBlueprintLibrary::GetPropertyTableName(PropertyTable);

  if (propertyTableName.IsEmpty()) {
    // Substitute the name with the property table's class.
    propertyTableName = PropertyTable.getClassName();
  }

  return propertyTableName;
}

FString getMaterialNameForPropertyTableProperty(
    const FString& propertyTableName,
    const FString& propertyName) {
  // Example: "PTABLE_houses_roofColor"
  return MaterialPropertyTablePrefix + propertyTableName + "_" + propertyName;
}

namespace {

struct EncodedPixelFormat {
  EPixelFormat format;
  size_t pixelSize;
};

// TODO: consider picking better pixel formats when they are available for the
// current platform.
EncodedPixelFormat
getPixelFormat(FCesiumMetadataEncodingDetails encodingDetails) {

  switch (encodingDetails.ComponentType) {
  case ECesiumEncodedMetadataComponentType::Uint8:
    switch (encodingDetails.Type) {
    case ECesiumEncodedMetadataType::Scalar:
      return {EPixelFormat::PF_R8_UINT, 1};
    case ECesiumEncodedMetadataType::Vec2:
    case ECesiumEncodedMetadataType::Vec3:
    case ECesiumEncodedMetadataType::Vec4:
      return {EPixelFormat::PF_R8G8B8A8_UINT, 4};
    default:
      return {EPixelFormat::PF_Unknown, 0};
    }
  case ECesiumEncodedMetadataComponentType::Float:
    switch (encodingDetails.Type) {
    case ECesiumEncodedMetadataType::Scalar:
      return {EPixelFormat::PF_R32_FLOAT, 4};
    case ECesiumEncodedMetadataType::Vec2:
    case ECesiumEncodedMetadataType::Vec3:
    case ECesiumEncodedMetadataType::Vec4:
      // Note this is ABGR
      return {EPixelFormat::PF_A32B32G32R32F, 16};
    }
  default:
    return {EPixelFormat::PF_Unknown, 0};
  }
}

bool isValidPropertyTablePropertyDescription(
    const FCesiumPropertyTablePropertyDescription& propertyDescription,
    const FCesiumPropertyTableProperty& property) {
  if (propertyDescription.EncodingDetails.Type ==
      ECesiumEncodedMetadataType::None) {
    UE_LOG(
        LogCesium,
        Warning,
        TEXT(
            "No encoded metadata type was specified for this property table property; skip encoding."));
    return false;
  }

  if (propertyDescription.EncodingDetails.ComponentType ==
      ECesiumEncodedMetadataComponentType::None) {
    UE_LOG(
        LogCesium,
        Warning,
        TEXT(
            "No encoded metadata component type was specified for this property table property; skip encoding."));
    return false;
  }

  const FCesiumMetadataValueType expectedType =
      propertyDescription.PropertyDetails.GetValueType();
  const FCesiumMetadataValueType valueType =
      UCesiumPropertyTablePropertyBlueprintLibrary::GetValueType(property);
  if (valueType != expectedType) {
    UE_LOG(
        LogCesium,
        Warning,
        TEXT(
            "The value type of the metadata property %s does not match the type specified by the metadata description. It will still attempt to be encoded, but may result in empty or unexpected values."),
        *propertyDescription.Name);
  }

  bool isNormalized =
      UCesiumPropertyTablePropertyBlueprintLibrary::IsNormalized(property);
  if (propertyDescription.PropertyDetails.bIsNormalized != isNormalized) {
    FString error =
        propertyDescription.PropertyDetails.bIsNormalized
            ? "Description incorrectly marked a property table property as normalized; skip encoding."
            : "Description incorrectly marked a property table property as not normalized; skip encoding.";
    UE_LOG(LogCesium, Warning, TEXT("%s"), *error);
    return false;
  }

  // Only uint8 normalization is currently supported.
  if (isNormalized &&
      valueType.ComponentType != ECesiumMetadataComponentType::Uint8) {
    UE_LOG(
        LogCesium,
        Warning,
        TEXT("Only normalization of uint8 properties is currently supported."));
    return false;
  }

  return true;
}

} // namespace

EncodedPropertyTable encodePropertyTableAnyThreadPart(
    const FCesiumPropertyTableDescription& propertyTableDescription,
    const FCesiumPropertyTable& propertyTable) {

  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodePropertyTable)

  EncodedPropertyTable encodedPropertyTable;

  int64 propertyTableCount =
      UCesiumPropertyTableBlueprintLibrary::GetPropertyTableCount(
          propertyTable);

  const TMap<FString, FCesiumPropertyTableProperty>& properties =
      UCesiumPropertyTableBlueprintLibrary::GetProperties(propertyTable);

  encodedPropertyTable.properties.Reserve(properties.Num());
  for (const auto& pair : properties) {
    const FCesiumPropertyTableProperty& property = pair.Value;

    const FCesiumPropertyTablePropertyDescription* pDescription =
        propertyTableDescription.Properties.FindByPredicate(
            [&key = pair.Key](const FCesiumPropertyTablePropertyDescription&
                                  expectedProperty) {
              return key == expectedProperty.Name;
            });

    if (!pDescription) {
      continue;
    }

    const FCesiumMetadataEncodingDetails& encodingDetails =
        pDescription->EncodingDetails;
    if (encodingDetails.Conversion == ECesiumEncodedMetadataConversion::None) {
      // No encoding to be done; skip.
      continue;
    }

    if (!isValidPropertyTablePropertyDescription(*pDescription, property)) {
      continue;
    }

    if (encodingDetails.Conversion ==
            ECesiumEncodedMetadataConversion::Coerce &&
        !CesiumEncodedMetadataCoerce::canEncode(*pDescription)) {
      UE_LOG(
          LogCesium,
          Warning,
          TEXT(
              "Cannot use 'Coerce' with the specified property info; skipped."));
      continue;
    }

    if (encodingDetails.Conversion ==
            ECesiumEncodedMetadataConversion::ParseColorFromString &&
        !CesiumEncodedMetadataParseColorFromString::canEncode(*pDescription)) {
      UE_LOG(
          LogCesium,
          Warning,
          TEXT(
              "Cannot use `Parse Color From String` with the specified property info; skipped."));
      continue;
    }

    EncodedPixelFormat encodedFormat = getPixelFormat(encodingDetails);
    if (encodedFormat.format == EPixelFormat::PF_Unknown) {
      UE_LOG(
          LogCesium,
          Warning,
          TEXT(
              "Unable to determine a suitable GPU format for this property table property; skipped."));
      continue;
    }

    TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodePropertyTableProperty)

    EncodedPropertyTableProperty& encodedProperty =
        encodedPropertyTable.properties.Emplace_GetRef();
    encodedProperty.name = createHlslSafeName(pDescription->Name);
    encodedProperty.type = pDescription->EncodingDetails.Type;

    if (UCesiumPropertyTablePropertyBlueprintLibrary::
            GetPropertyTablePropertyStatus(property) ==
        ECesiumPropertyTablePropertyStatus::Valid) {

      int64 floorSqrtFeatureCount = glm::sqrt(propertyTableCount);
      int64 textureDimension =
          (floorSqrtFeatureCount * floorSqrtFeatureCount == propertyTableCount)
              ? floorSqrtFeatureCount
              : (floorSqrtFeatureCount + 1);

      encodedProperty.pTexture = MakeUnique<LoadedTextureResult>();
      // TODO: upgrade to new texture creation path.
      encodedProperty.pTexture->textureSource = LegacyTextureSource{};
      encodedProperty.pTexture->pTextureData = createTexturePlatformData(
          textureDimension,
          textureDimension,
          encodedFormat.format);

      encodedProperty.pTexture->addressX = TextureAddress::TA_Clamp;
      encodedProperty.pTexture->addressY = TextureAddress::TA_Clamp;
      encodedProperty.pTexture->filter = TextureFilter::TF_Nearest;

      if (!encodedProperty.pTexture->pTextureData) {
        UE_LOG(
            LogCesium,
            Error,
            TEXT(
                "Error encoding a property table property. Most likely could not allocate enough texture memory."));
        continue;
      }

      FTexture2DMipMap* pMip = new FTexture2DMipMap();
      encodedProperty.pTexture->pTextureData->Mips.Add(pMip);
      pMip->SizeX = textureDimension;
      pMip->SizeY = textureDimension;

      pMip->BulkData.Lock(LOCK_READ_WRITE);

      void* pTextureData = pMip->BulkData.Realloc(
          textureDimension * textureDimension * encodedFormat.pixelSize);

      if (encodingDetails.Conversion ==
          ECesiumEncodedMetadataConversion::ParseColorFromString) {
        CesiumEncodedMetadataParseColorFromString::encode(
            *pDescription,
            property,
            pTextureData,
            encodedFormat.pixelSize);
      } else /* info.Conversion == ECesiumEncodedMetadataConversion::Coerce */ {
        CesiumEncodedMetadataCoerce::encode(
            *pDescription,
            property,
            pTextureData,
            encodedFormat.pixelSize);
      }
      pMip->BulkData.Unlock();
    }

    if (pDescription->PropertyDetails.bHasOffset) {
      encodedProperty.offset =
          UCesiumPropertyTablePropertyBlueprintLibrary::GetOffset(property);
    }

    if (pDescription->PropertyDetails.bHasScale) {
      encodedProperty.scale =
          UCesiumPropertyTablePropertyBlueprintLibrary::GetScale(property);
    }

    if (pDescription->PropertyDetails.bHasNoDataValue) {
      encodedProperty.noData =
          UCesiumPropertyTablePropertyBlueprintLibrary::GetNoDataValue(
              property);
    }

    if (pDescription->PropertyDetails.bHasDefaultValue) {
      encodedProperty.defaultValue =
          UCesiumPropertyTablePropertyBlueprintLibrary::GetDefaultValue(
              property);
    }
  }

  return encodedPropertyTable;
}

EncodedPropertyTexture encodePropertyTextureAnyThreadPart(
    TMap<const CesiumGltf::ImageCesium*, TWeakPtr<LoadedTextureResult>>&
        featureTexturePropertyMap,
    const FCesiumPropertyTextureDescription& propertyTextureDescription,
    const FString& featureTextureName,
    const FCesiumPropertyTexture& propertyTexture) {

  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeFeatureTexture)

  EncodedPropertyTexture encodedPropertyTexture;

  // const CesiumGltf::PropertyTextureView& propertyTextureView =
  //    propertyTexture.getPropertyTextureView();
  // const std::unordered_map<std::string,
  // CesiumGltf::FeatureTexturePropertyView>&
  //    properties = featureTextureView.getProperties();

  // encodedFeatureTexture.properties.Reserve(properties.size());

  // for (const auto& propertyIt : properties) {
  //  const FFeatureTexturePropertyDescription* pPropertyDescription =
  //      featureTextureDescription.Properties.FindByPredicate(
  //          [propertyName = UTF8_TO_TCHAR(propertyIt.first.c_str())](
  //              const FFeatureTexturePropertyDescription& expectedProperty)
  //              {
  //            return propertyName == expectedProperty.Name;
  //          });

  //  if (!pPropertyDescription) {
  //    continue;
  //  }

  //  const CesiumGltf::FeatureTexturePropertyView& featureTexturePropertyView
  //  =
  //      propertyIt.second;

  //  const CesiumGltf::ImageCesium* pImage =
  //      featureTexturePropertyView.getImage();

  //  if (!pImage) {
  //    UE_LOG(
  //        LogCesium,
  //        Warning,
  //        TEXT("This feature texture property does not have a valid
  //        image."));
  //    continue;
  //  }

  //  int32 expectedComponentCount = 1;
  //  switch (pPropertyDescription->Type) {
  //  // case ECesiumPropertyType::Scalar:
  //  //  expectedComponentCount = 1;
  //  //  break;
  //  case ECesiumPropertyType::Vec2:
  //    expectedComponentCount = 2;
  //    break;
  //  case ECesiumPropertyType::Vec3:
  //    expectedComponentCount = 3;
  //    break;
  //  case ECesiumPropertyType::Vec4:
  //    expectedComponentCount = 4;
  //  };

  //  if (expectedComponentCount != propertyIt.second.getComponentCount() ||
  //      pPropertyDescription->Normalized != propertyIt.second.isNormalized()
  //      || pPropertyDescription->Swizzle !=
  //          UTF8_TO_TCHAR(propertyIt.second.getSwizzle().c_str())) {
  //    UE_LOG(
  //        LogCesium,
  //        Warning,
  //        TEXT(
  //            "This feature texture property does not have the expected
  //            component count, normalization, or swizzle string."));
  //    continue;
  //  }

  //  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeFeatureTextureProperty)

  //  EncodedFeatureTextureProperty& encodedFeatureTextureProperty =
  //      encodedFeatureTexture.properties.Emplace_GetRef();

  //  encodedFeatureTextureProperty.baseName =
  //      "FTX_" + featureTextureName + "_" + pPropertyDescription->Name +
  //      "_";
  //  encodedFeatureTextureProperty.textureCoordinateAttributeId =
  //      featureTexturePropertyView.getTextureCoordinateAttributeId();

  //  const CesiumGltf::FeatureTexturePropertyChannelOffsets& channelOffsets =
  //      featureTexturePropertyView.getChannelOffsets();
  //  encodedFeatureTextureProperty.channelOffsets[0] = channelOffsets.r;
  //  encodedFeatureTextureProperty.channelOffsets[1] = channelOffsets.g;
  //  encodedFeatureTextureProperty.channelOffsets[2] = channelOffsets.b;
  //  encodedFeatureTextureProperty.channelOffsets[3] = channelOffsets.a;

  //  TWeakPtr<LoadedTextureResult>* pMappedUnrealImageIt =
  //      featureTexturePropertyMap.Find(pImage);
  //  if (pMappedUnrealImageIt) {
  //    encodedFeatureTextureProperty.pTexture = pMappedUnrealImageIt->Pin();
  //  } else {
  //    encodedFeatureTextureProperty.pTexture =
  //        MakeShared<LoadedTextureResult>();
  //    // TODO: upgrade to new texture creation path.
  //    encodedFeatureTextureProperty.pTexture->textureSource =
  //        LegacyTextureSource{};
  //    featureTexturePropertyMap.Emplace(
  //        pImage,
  //        encodedFeatureTextureProperty.pTexture);
  //    encodedFeatureTextureProperty.pTexture->pTextureData =
  //        createTexturePlatformData(
  //            pImage->width,
  //            pImage->height,
  //            // TODO: currently the unnormalized pixels are always in
  //            // unsigned R8G8B8A8 form, but this does not necessarily need
  //            // to be the case in the future.
  //            featureTexturePropertyView.isNormalized()
  //                ? EPixelFormat::PF_R8G8B8A8
  //                : EPixelFormat::PF_R8G8B8A8_UINT);

  //    encodedFeatureTextureProperty.pTexture->addressX =
  //        TextureAddress::TA_Clamp;
  //    encodedFeatureTextureProperty.pTexture->addressY =
  //        TextureAddress::TA_Clamp;
  //    encodedFeatureTextureProperty.pTexture->filter =
  //        TextureFilter::TF_Nearest;

  //    if (!encodedFeatureTextureProperty.pTexture->pTextureData) {
  //      UE_LOG(
  //          LogCesium,
  //          Error,
  //          TEXT(
  //              "Error encoding a feature table property. Most likely could
  //              not allocate enough texture memory."));
  //      continue;
  //    }

  //    FTexture2DMipMap* pMip = new FTexture2DMipMap();
  //    encodedFeatureTextureProperty.pTexture->pTextureData->Mips.Add(pMip);
  //    pMip->SizeX = pImage->width;
  //    pMip->SizeY = pImage->height;
  //    pMip->BulkData.Lock(LOCK_READ_WRITE);

  //    void* pTextureData = pMip->BulkData.Realloc(pImage->pixelData.size());

  //    FMemory::Memcpy(
  //        pTextureData,
  //        pImage->pixelData.data(),
  //        pImage->pixelData.size());

  //    pMip->BulkData.Unlock();
  //  }
  //}

  return encodedPropertyTexture;
}

EncodedPrimitiveMetadata encodePrimitiveMetadataAnyThreadPart(
    const FCesiumModelMetadataDescription& metadataDescription,
    const FCesiumPrimitiveFeatures& features,
    const FCesiumPrimitiveMetadata& primitive) {

  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeMetadataPrimitive)

  EncodedPrimitiveMetadata result;

  // const TArray<FString>& featureTextureNames =
  //    UCesiumMetadataPrimitiveBlueprintLibrary::GetFeatureTextureNames(
  //        primitive);
  // result.featureTextureNames.Reserve(featureTextureNames.Num());

  // for (const FFeatureTextureDescription& expectedFeatureTexture :
  //     metadataDescription.FeatureTextures) {
  //  if (featureTextureNames.Find(expectedFeatureTexture.Name) != INDEX_NONE)
  //  {
  //    result.featureTextureNames.Add(expectedFeatureTexture.Name);
  //  }
  //}

  return result;
}

EncodedModelMetadata encodeModelMetadataAnyThreadPart(
    const FCesiumModelMetadataDescription& metadataDescription,
    const FCesiumModelMetadata& metadata) {

  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeModelMetadata)

  EncodedModelMetadata result;

  const TArray<FCesiumPropertyTable>& propertyTables =
      UCesiumModelMetadataBlueprintLibrary::GetPropertyTables(metadata);
  result.propertyTables.Reserve(propertyTables.Num());
  for (const auto& propertyTable : propertyTables) {
    const FString propertyTableName = getNameForPropertyTable(propertyTable);

    const FCesiumPropertyTableDescription* pExpectedPropertyTable =
        metadataDescription.PropertyTables.FindByPredicate(
            [&propertyTableName](
                const FCesiumPropertyTableDescription& expectedPropertyTable) {
              return propertyTableName == expectedPropertyTable.Name;
            });

    if (pExpectedPropertyTable) {
      TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodePropertyTable)

      auto& encodedPropertyTable =
          result.propertyTables.Emplace_GetRef(encodePropertyTableAnyThreadPart(
              *pExpectedPropertyTable,
              propertyTable));
      encodedPropertyTable.name = propertyTableName;
    }
  }

  const TArray<FCesiumPropertyTexture>& propertyTextures =
      UCesiumModelMetadataBlueprintLibrary::GetPropertyTextures(metadata);
  result.propertyTextures.Reserve(propertyTextures.Num());
  TMap<const CesiumGltf::ImageCesium*, TWeakPtr<LoadedTextureResult>>
      propertyTexturePropertyMap;
  propertyTexturePropertyMap.Reserve(propertyTextures.Num());
  for (const auto& propertyTextureIt : propertyTextures) {
    // const FString& featureTextureName = featureTextureIt.Key;

    // const FFeatureTextureDescription* pExpectedFeatureTexture =
    //    metadataDescription.FeatureTextures.FindByPredicate(
    //        [&featureTextureName](
    //            const FFeatureTextureDescription& expectedFeatureTexture) {
    //          return featureTextureName == expectedFeatureTexture.Name;
    //        });

    // if (pExpectedFeatureTexture) {
    //  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeFeatureTexture)

    //  result.encodedFeatureTextures.Emplace(
    //      featureTextureName,
    //      encodeFeatureTextureAnyThreadPart(
    //          featureTexturePropertyMap,
    //          *pExpectedFeatureTexture,
    //          featureTextureName,
    //          featureTextureIt.Value));
    //}
  }

  return result;
}

bool encodePropertyTableGameThreadPart(
    EncodedPropertyTable& encodedPropertyTable) {
  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodePropertyTable)

  bool success = true;

  for (EncodedPropertyTableProperty& encodedProperty :
       encodedPropertyTable.properties) {
    if (encodedProperty.pTexture) {
      success &=
          loadTextureGameThreadPart(encodedProperty.pTexture.Get()) != nullptr;
    }
  }

  return success;
}

bool encodePropertyTextureGameThreadPart(
    TArray<LoadedTextureResult*>& uniqueTextures,
    EncodedPropertyTexture& encodedPropertyTexture) {
  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodePropertyTexture)

  bool success = true;

  for (EncodedPropertyTextureProperty& property :
       encodedPropertyTexture.properties) {
    if (uniqueTextures.Find(property.pTexture.Get()) == INDEX_NONE) {
      success &= loadTextureGameThreadPart(property.pTexture.Get()) != nullptr;
      uniqueTextures.Emplace(property.pTexture.Get());
    }
  }

  return success;
}

bool encodePrimitiveMetadataGameThreadPart(
    EncodedPrimitiveMetadata& encodedPrimitive) {
  bool success = true;

  // TArray<const LoadedTextureResult*> uniqueFeatureIdImages;
  // uniqueFeatureIdImages.Reserve(
  //    encodedPrimitive.encodedFeatureIdTextures.Num());

  // for (EncodedFeatureIdTexture& encodedFeatureIdTexture :
  //     encodedPrimitive.encodedFeatureIdTextures) {
  //  if (uniqueFeatureIdImages.Find(encodedFeatureIdTexture.pTexture.Get())
  //  ==
  //      INDEX_NONE) {
  //    success &= loadTextureGameThreadPart(
  //                   encodedFeatureIdTexture.pTexture.Get()) != nullptr;
  //    uniqueFeatureIdImages.Emplace(encodedFeatureIdTexture.pTexture.Get());
  //  }
  //}

  return success;
}

bool encodeModelMetadataGameThreadPart(EncodedModelMetadata& encodedMetadata) {
  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::EncodeMetadata)

  bool success = true;

  TArray<LoadedTextureResult*> uniqueTextures;
  uniqueTextures.Reserve(encodedMetadata.propertyTextures.Num());
  for (auto& encodedPropertyTextureIt : encodedMetadata.propertyTextures) {
    success &= encodePropertyTextureGameThreadPart(
        uniqueTextures,
        encodedPropertyTextureIt);
  }

  for (auto& encodedPropertyTable : encodedMetadata.propertyTables) {
    success &= encodePropertyTableGameThreadPart(encodedPropertyTable);
  }

  return success;
}

void destroyEncodedPrimitiveMetadata(
    EncodedPrimitiveMetadata& encodedPrimitive) {
  // for (EncodedFeatureIdTexture& encodedFeatureIdTexture :
  //     encodedPrimitive.encodedFeatureIdTextures) {

  //  if (encodedFeatureIdTexture.pTexture->pTexture.IsValid()) {
  //    CesiumLifetime::destroy(encodedFeatureIdTexture.pTexture->pTexture.Get());
  //    encodedFeatureIdTexture.pTexture->pTexture.Reset();
  //  }
  //}
}

void destroyEncodedModelMetadata(EncodedModelMetadata& encodedMetadata) {
  for (auto& propertyTable : encodedMetadata.propertyTables) {
    for (EncodedPropertyTableProperty& encodedProperty :
         propertyTable.properties) {
      if (encodedProperty.pTexture &&
          encodedProperty.pTexture->pTexture.IsValid()) {
        CesiumLifetime::destroy(encodedProperty.pTexture->pTexture.Get());
        encodedProperty.pTexture->pTexture.Reset();
      }
    }
  }

  for (auto& encodedPropertyTextureIt : encodedMetadata.propertyTextures) {
    for (EncodedPropertyTextureProperty& encodedPropertyTextureProperty :
         encodedPropertyTextureIt.properties) {
      if (encodedPropertyTextureProperty.pTexture->pTexture.IsValid()) {
        CesiumLifetime::destroy(
            encodedPropertyTextureProperty.pTexture->pTexture.Get());
        encodedPropertyTextureProperty.pTexture->pTexture.Reset();
      }
    }
  }
}

// The result should be a safe hlsl identifier, but any name clashes after
// fixing safety will not be automatically handled.
FString createHlslSafeName(const FString& rawName) {
  static const FString identifierHeadChar =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_";
  static const FString identifierTailChar = identifierHeadChar + "0123456789";

  FString safeName = rawName;
  int32 _;
  if (safeName.Len() == 0) {
    return "_";
  } else {
    if (!identifierHeadChar.FindChar(safeName[0], _)) {
      safeName = "_" + safeName;
    }
  }

  for (size_t i = 1; i < safeName.Len(); ++i) {
    if (!identifierTailChar.FindChar(safeName[i], _)) {
      safeName[i] = '_';
    }
  }

  return safeName;
}

} // namespace CesiumEncodedFeaturesMetadata
