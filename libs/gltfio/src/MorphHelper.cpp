/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MorphHelper.h"

#include <filament/RenderableManager.h>
#include <filament/VertexBuffer.h>
#include <utils/JobSystem.h>
#include <math/norm.h>
#include <filament/MorphTargetBuffer.h>

#include "TangentsJob.h"

using namespace filament;
using namespace filament::math;
using namespace utils;

static constexpr uint8_t kUnused = 0xff;

namespace gltfio {

uint32_t computeBindingSize(const cgltf_accessor* accessor);
uint32_t computeBindingOffset(const cgltf_accessor* accessor);

static const auto FREE_CALLBACK = [](void* mem, size_t, void*) { free(mem); };

// Returns true if a is a subset of b.
static bool isSubsetOf(ubyte4 a, ubyte4 b) {
    if (a.x != kUnused && a.x != b.x && a.x != b.y && a.x != b.z && a.x != b.w) return false;
    if (a.y != kUnused && a.y != b.x && a.y != b.y && a.y != b.z && a.y != b.w) return false;
    if (a.z != kUnused && a.z != b.x && a.z != b.y && a.z != b.z && a.z != b.w) return false;
    if (a.w != kUnused && a.w != b.x && a.w != b.y && a.w != b.z && a.w != b.w) return false;
    return true;
}

MorphHelper::MorphHelper(FFilamentAsset* asset, FFilamentInstance* inst) : mAsset(asset),
        mInstance(inst) {
    NodeMap& sourceNodes = asset->isInstanced() ? asset->mInstances[0]->nodeMap : asset->mNodeMap;
    for (auto pair : sourceNodes) {
        cgltf_node const* node = pair.first;
        cgltf_mesh const* mesh = node->mesh;
        if (mesh) {
            cgltf_primitive const* prims = mesh->primitives;
            for (cgltf_size pi = 0, count = mesh->primitives_count; pi < count; ++pi) {
                addPrimitive(mesh, pi, pair.second);
            }
            addTargetNames(mesh, pair.second);
        }
    }
}

MorphHelper::~MorphHelper() {
    auto engine = mAsset->mEngine;
    for (auto& entry : mMorphTable) {
        for (auto& prim : entry.second.primitives) {
            if (prim.targets) {
                engine->destroy(prim.targets);
            }
        }
    }
}

const char* MorphHelper::getTargetNameAt(Entity entity, size_t targetIndex) const noexcept {
    if (mMorphTable.count(entity)) {
        auto& targetNames = mMorphTable.at(entity).targetNames;
        if (!targetNames.empty()) {
            return targetNames[targetIndex].c_str();
        }
    }
    return nullptr;
}

// This method copies various morphing-related data from the FilamentAsset MeshCache primitive
// (which lives in transient memory) into the MorphHelper primitive (which will stay resident).
void MorphHelper::addPrimitive(cgltf_mesh const* mesh, int primitiveIndex, Entity entity){
    auto& entry = mMorphTable[entity];
    auto& engine = *mAsset->mEngine;
    const cgltf_primitive& prim = mesh->primitives[primitiveIndex];
    const auto& gltfioPrim = mAsset->mMeshCache.at(mesh)[primitiveIndex];
    VertexBuffer* vertexBuffer = gltfioPrim.vertices;
    entry.primitives.push_back({ nullptr });

    auto& morphHelperPrim = entry.primitives.back();

    if (prim.targets_count) {
        morphHelperPrim.targets = MorphTargetBuffer::Builder()
                .vertexCount(vertexBuffer->getVertexCount())
                .count(prim.targets_count)
                .build(engine);

        auto& rcm = engine.getRenderableManager();
        rcm.setMorphTargetBufferAt(rcm.getInstance(entity), 0, primitiveIndex,
                morphHelperPrim.targets, vertexBuffer->getVertexCount());
    }

    const cgltf_accessor* previous = nullptr;
    for (int targetIndex = 0; targetIndex < prim.targets_count; targetIndex++) {
        const cgltf_morph_target& morphTarget = prim.targets[targetIndex];
        bool hasNormals = false;
        for (cgltf_size aindex = 0; aindex < morphTarget.attributes_count; aindex++) {
            const cgltf_attribute& attribute = morphTarget.attributes[aindex];
            const cgltf_accessor* accessor = attribute.data;
            const cgltf_attribute_type atype = attribute.type;
            if (atype == cgltf_attribute_type_tangent) {
                continue;
            }
            if (atype == cgltf_attribute_type_normal) {
                hasNormals = true;
                // TODO: use JobSystem for this, like what we do for non-morph tangents.
                TangentsJob job;
                TangentsJob::Params params = { .in = { &prim, targetIndex } };
                TangentsJob::run(&params);

                if (params.out.results) {
                    morphHelperPrim.targets->setTangentsAt(engine, targetIndex,
                            params.out.results, params.out.vertexCount);
                    free(params.out.results);
                }
                continue;
            }
            if (atype == cgltf_attribute_type_position) {
                // All position attributes must have the same data type.
                assert_invariant(!previous || previous->component_type == accessor->component_type);
                assert_invariant(!previous || previous->type == accessor->type);
                previous = accessor;

                // This should always be non-null, but don't crash if the glTF is malformed.
                if (accessor->buffer_view) {
                    auto bufferData = (const uint8_t*) accessor->buffer_view->buffer->data;
                    assert_invariant(bufferData);
                    auto* data = computeBindingOffset(accessor) + bufferData;

                    if (accessor->type == cgltf_type_vec3) {
                        morphHelperPrim.targets->setPositionsAt(engine, targetIndex,
                                (const float3*) data, vertexBuffer->getVertexCount());
                    } else {
                        assert_invariant(accessor->type == cgltf_type_vec4);
                        morphHelperPrim.targets->setPositionsAt(engine, targetIndex,
                                (const float4*) data, vertexBuffer->getVertexCount());
                    }
                }
            }
        }

        // Generate flat normals if necessary.
        if (!hasNormals && !prim.material->unlit) {
            TangentsJob job;
            TangentsJob::Params params = { .in = { &prim, targetIndex } };
            TangentsJob::run(&params);
            if (params.out.results) {
                morphHelperPrim.targets->setTangentsAt(engine, targetIndex,
                        params.out.results, params.out.vertexCount);
                free(params.out.results);
            }
        }
    }
}

void MorphHelper::addTargetNames(cgltf_mesh const* mesh, Entity entity) {
    const auto count = mesh->target_names_count;
    if (count == 0) {
        return;
    }

    auto& entry = mMorphTable[entity];
    auto& names = entry.targetNames;

    assert_invariant(names.empty());
    names.reserve(count);

    for (cgltf_size i = 0; i < count; ++i) {
        names.push_back(utils::StaticString::make(mesh->target_names[i]));
    }
}

}  // namespace gltfio
