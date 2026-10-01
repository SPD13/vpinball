// license:GPLv3+

#include "core/stdafx.h"

#if defined(ENABLE_XR)

#include "VRControllerModels.h"
#include "renderer/IndexBuffer.h"
#include "renderer/MeshBuffer.h"
#include "renderer/Renderer.h"
#include "renderer/Texture.h"
#include "renderer/VertexBuffer.h"
#include "renderer/VRDevice.h"
#include "utils/color.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

// Controllers stay readable under the dim lighting of many tables: part of their color is shown unlit
static constexpr float controllerUnlitPart = 0.5f;

// glTF metallic-roughness material to a VPX material, colors given in linear space (the material colors of VPX are sRGB)
static Material ToMaterial(const float (&baseColor)[4], float metallic, float roughness, bool transparent)
{
   const auto toSRGB = [](float c) { return static_cast<int>(powf(clamp(c, 0.f, 1.f), 1.f / 2.2f) * 255.f + 0.5f); };
   Material mat;
   mat.m_type = metallic > 0.5f ? Material::METAL : Material::BASIC;
   mat.m_cBase = RGB(toSRGB(baseColor[0]), toSRGB(baseColor[1]), toSRGB(baseColor[2]));
   const float shininess = clamp(1.f - roughness, 0.f, 1.f);
   mat.m_fRoughness = shininess; // The roughness of VPX materials is a shininess: 1 gives the sharpest highlight
   const int glossy = static_cast<int>(shininess * 0.25f * 255.f);
   mat.m_cGlossy = RGB(glossy, glossy, glossy);
   mat.m_cClearcoat = 0;
   mat.m_fGlossyImageLerp = 1.f;
   mat.m_fWrapLighting = 0.5f;
   mat.m_fEdge = 1.f;
   mat.m_fEdgeAlpha = 1.f;
   mat.m_fThickness = 0.05f;
   mat.m_fOpacity = clamp(baseColor[3], 0.f, 1.f);
   mat.m_bOpacityActive = transparent;
   return mat;
}

VRControllerModels::VRControllerModels(RenderDevice* rd)
   : m_rd(rd)
{
}

VRControllerModels::~VRControllerModels()
{
   for (const auto& model : m_models)
      Release(*model);
}

void VRControllerModels::Release(Model& model) const
{
   for (const auto& texture : model.textures)
      if (texture)
         m_rd->m_texMan.UnloadTexture(texture.get());
   model.textures.clear();
   model.primitives.clear();
}

std::unique_ptr<VRControllerModels::Model> VRControllerModels::Load(uint64_t id, const vector<uint8_t>& glb, const vector<string>& animatableNodes) const
{
   cgltf_options options {};
   cgltf_data* data = nullptr;
   if (cgltf_parse(&options, glb.data(), glb.size(), &data) != cgltf_result_success)
   {
      PLOGE << "OpenXR controller model " << id << ": invalid glTF asset";
      return nullptr;
   }
   // The buffer of a glTF binary is its BIN chunk: no file is read
   if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success)
   {
      PLOGE << "OpenXR controller model " << id << ": glTF buffers not found";
      cgltf_free(data);
      return nullptr;
   }

   auto model = std::make_unique<Model>();
   model->id = id;

   // Textures: images stored in the binary (PNG or JPEG)
   model->textures.resize(data->images_count);
   for (cgltf_size i = 0; i < data->images_count; i++)
   {
      const cgltf_image& image = data->images[i];
      if (const uint8_t* bytes = image.buffer_view ? cgltf_buffer_view_data(image.buffer_view) : nullptr; bytes)
         model->textures[i] = BaseTexture::CreateFromData(bytes, image.buffer_view->size);
      if (model->textures[i] == nullptr)
         PLOGW << "OpenXR controller model " << id << ": image " << i << " not loaded (" << (image.mime_type ? image.mime_type : "external file") << ')';
   }

   // Meshes: one mesh buffer per triangle primitive
   vector<vector<int>> meshPrimitives(data->meshes_count);
   for (cgltf_size m = 0; m < data->meshes_count; m++)
   {
      const cgltf_mesh& mesh = data->meshes[m];
      for (cgltf_size p = 0; p < mesh.primitives_count; p++)
      {
         const cgltf_primitive& primitive = mesh.primitives[p];
         if (primitive.type != cgltf_primitive_type_triangles)
            continue;
         const cgltf_accessor* position = nullptr;
         const cgltf_accessor* normal = nullptr;
         const cgltf_accessor* uv = nullptr;
         for (cgltf_size a = 0; a < primitive.attributes_count; a++)
         {
            const cgltf_attribute& attribute = primitive.attributes[a];
            if (attribute.type == cgltf_attribute_type_position)
               position = attribute.data;
            else if (attribute.type == cgltf_attribute_type_normal)
               normal = attribute.data;
            else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
               uv = attribute.data;
         }
         if (position == nullptr || position->count == 0)
            continue;

         vector<Vertex3D_NoTex2> vertices(position->count);
         for (cgltf_size v = 0; v < position->count; v++)
         {
            float value[3] = { 0.f, 0.f, 0.f };
            cgltf_accessor_read_float(position, v, value, 3);
            vertices[v].x = value[0];
            vertices[v].y = value[1];
            vertices[v].z = value[2];
            value[0] = value[1] = value[2] = 0.f;
            if (normal)
               cgltf_accessor_read_float(normal, v, value, 3);
            vertices[v].nx = value[0];
            vertices[v].ny = value[1];
            vertices[v].nz = value[2];
            value[0] = value[1] = 0.f;
            if (uv)
               cgltf_accessor_read_float(uv, v, value, 2);
            vertices[v].tu = value[0];
            vertices[v].tv = value[1];
         }
         vector<unsigned int> indices(primitive.indices ? primitive.indices->count : position->count);
         for (size_t i = 0; i < indices.size(); i++)
            indices[i] = primitive.indices ? static_cast<unsigned int>(cgltf_accessor_read_index(primitive.indices, i)) : static_cast<unsigned int>(i);
         indices.resize(indices.size() - indices.size() % 3);
         if (indices.empty() || std::ranges::any_of(indices, [&](unsigned int i) { return i >= vertices.size(); }))
            continue;
         if (normal == nullptr)
         {
            // Smooth normals from the faces, as glTF asks for flat ones but this is only for display
            for (size_t i = 0; i < indices.size(); i += 3)
            {
               Vertex3D_NoTex2& a = vertices[indices[i]];
               Vertex3D_NoTex2& b = vertices[indices[i + 1]];
               Vertex3D_NoTex2& c = vertices[indices[i + 2]];
               const vec3 n = CrossProduct(vec3(b.x - a.x, b.y - a.y, b.z - a.z), vec3(c.x - a.x, c.y - a.y, c.z - a.z));
               for (Vertex3D_NoTex2* vertex : { &a, &b, &c })
               {
                  vertex->nx += n.x;
                  vertex->ny += n.y;
                  vertex->nz += n.z;
               }
            }
            for (Vertex3D_NoTex2& vertex : vertices)
            {
               vec3 n(vertex.nx, vertex.ny, vertex.nz);
               if (n.LengthSquared() > 0.f)
                  n.Normalize();
               vertex.nx = n.x;
               vertex.ny = n.y;
               vertex.nz = n.z;
            }
         }

         Primitive out;
         auto vb = std::make_shared<VertexBuffer>(m_rd, static_cast<unsigned int>(vertices.size()), reinterpret_cast<const float*>(vertices.data()));
         auto ib = std::make_shared<IndexBuffer>(m_rd, indices);
         out.mesh = std::make_shared<MeshBuffer>(std::format("VRController.{}.{}.{}", id, m, p), vb, ib, true);

         // Without material, glTF uses a white, fully rough material. The metallic-roughness texture is not used (only factors apply per mesh):
         // a mesh which has one is shaded as a non metal, since its metallic factor then usually only scales the texture.
         float baseColor[4] = { 1.f, 1.f, 1.f, 1.f };
         float metallic = 0.f, roughness = 1.f;
         if (const cgltf_material* material = primitive.material; material)
         {
            if (material->has_pbr_metallic_roughness)
            {
               const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
               std::copy(std::begin(pbr.base_color_factor), std::end(pbr.base_color_factor), baseColor);
               metallic = pbr.metallic_roughness_texture.texture ? 0.f : pbr.metallic_factor;
               roughness = pbr.roughness_factor;
               if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image)
                  out.texture = static_cast<int>(cgltf_image_index(data, pbr.base_color_texture.texture->image));
            }
            if (material->alpha_mode == cgltf_alpha_mode_mask)
               out.alphaTest = material->alpha_cutoff;
            out.transparent = material->alpha_mode == cgltf_alpha_mode_blend;
         }
         out.material = ToMaterial(baseColor, metallic, roughness, out.transparent);
         meshPrimitives[m].push_back(static_cast<int>(model->primitives.size()));
         model->primitives.push_back(std::move(out));
      }
   }

   // Nodes, sorted parents first, from the roots of the scene (or of the whole asset when it has no scene)
   vector<const cgltf_node*> stack;
   if (const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr); scene)
      for (cgltf_size i = scene->nodes_count; i > 0; i--)
         stack.push_back(scene->nodes[i - 1]);
   else
      for (cgltf_size i = data->nodes_count; i > 0; i--)
         if (data->nodes[i - 1].parent == nullptr)
            stack.push_back(&data->nodes[i - 1]);
   vector<int> nodeIndex(data->nodes_count, -1);
   vector<bool> animatableFound(animatableNodes.size(), false);
   while (!stack.empty())
   {
      const cgltf_node* const node = stack.back();
      stack.pop_back();
      const cgltf_size index = cgltf_node_index(data, node);
      if (nodeIndex[index] >= 0) // Malformed asset with a node used twice
         continue;
      nodeIndex[index] = static_cast<int>(model->nodes.size());
      Node out;
      out.parent = node->parent ? nodeIndex[cgltf_node_index(data, node->parent)] : -1;
      // The column-major matrix of glTF (for column vectors) has the memory layout of our row-major matrix for row vectors
      cgltf_float local[16];
      cgltf_node_transform_local(node, local);
      memcpy(&out.local.m[0][0], local, sizeof(local));
      if (node->has_scale)
         out.scale = vec3(node->scale[0], node->scale[1], node->scale[2]);
      else if (node->has_matrix)
         out.scale = vec3(vec3(local[0], local[1], local[2]).Length(), vec3(local[4], local[5], local[6]).Length(), vec3(local[8], local[9], local[10]).Length());
      if (node->name)
         if (const auto it = std::ranges::find(animatableNodes, node->name); it != animatableNodes.end())
         {
            out.animatable = static_cast<int>(it - animatableNodes.begin());
            animatableFound[out.animatable] = true;
         }
      if (node->mesh)
         out.primitives = meshPrimitives[cgltf_mesh_index(data, node->mesh)];
      model->nodes.push_back(std::move(out));
      for (cgltf_size i = node->children_count; i > 0; i--)
         stack.push_back(node->children[i - 1]);
   }
   for (size_t i = 0; i < animatableNodes.size(); i++)
      if (!animatableFound[i])
         PLOGW << "OpenXR controller model " << id << ": animatable node '" << animatableNodes[i] << "' not found in the asset";

   PLOGI << "OpenXR controller model " << id << ": " << model->nodes.size() << " nodes, " << model->primitives.size() << " meshes, " << data->images_count << " textures";
   cgltf_free(data);
   return model;
}

void VRControllerModels::Render(Renderer* renderer, const VRDevice& vrDevice)
{
   const vector<VRDevice::ControllerModel>& controllers = vrDevice.GetControllerModels();

   // Follow the models listed by the runtime: release the ones it dropped, load the new ones
   std::erase_if(m_models, [&](const std::unique_ptr<Model>& model)
      {
         if (std::ranges::any_of(controllers, [&](const VRDevice::ControllerModel& controller) { return controller.id == model->id; }))
            return false;
         Release(*model);
         return true;
      });
   for (const VRDevice::ControllerModel& controller : controllers)
   {
      if (controller.asset == nullptr || std::ranges::find(m_failedIds, controller.id) != m_failedIds.end()
         || std::ranges::any_of(m_models, [&](const std::unique_ptr<Model>& model) { return model->id == controller.id; }))
         continue;
      if (auto model = Load(controller.id, *controller.asset, controller.animatableNodes); model)
         m_models.push_back(std::move(model));
      else
         m_failedIds.push_back(controller.id);
   }

   Shader* const shader = m_rd->m_basicShader;
   bool drawn = false;
   for (const VRDevice::ControllerModel& controller : controllers)
   {
      if (!controller.located)
         continue;
      const auto it = std::ranges::find_if(m_models, [&](const std::unique_ptr<Model>& model) { return model->id == controller.id; });
      if (it == m_models.end())
         continue;
      const Model& model = **it;

      // Place the nodes, with the poses of the animated ones given by the runtime (relative to their parent, the node keeping its scale)
      const Matrix3D modelToRoom = controller.modelToReference * vrDevice.GetReferenceToRoom();
      m_nodeWorld.resize(model.nodes.size());
      m_nodeVisible.resize(model.nodes.size());
      for (size_t i = 0; i < model.nodes.size(); i++)
      {
         const Node& node = model.nodes[i];
         Matrix3D local = node.local;
         bool visible = node.parent < 0 || m_nodeVisible[node.parent];
         if (node.animatable >= 0 && node.animatable < static_cast<int>(controller.nodeStates.size()))
         {
            const VRDevice::ControllerModel::NodeState& state = controller.nodeStates[node.animatable];
            local = Matrix3D::MatrixScale(node.scale.x, node.scale.y, node.scale.z) * VRDevice::PoseToMatrix(state.pose);
            visible = visible && state.visible;
         }
         m_nodeWorld[i] = node.parent < 0 ? local : local * m_nodeWorld[node.parent];
         m_nodeVisible[i] = visible;
      }

      for (size_t i = 0; i < model.nodes.size(); i++)
      {
         if (!m_nodeVisible[i] || model.nodes[i].primitives.empty())
            continue;
         const Matrix3D objectToRoom = m_nodeWorld[i] * modelToRoom;
         const Vertex3Ds center = objectToRoom * Vertex3Ds(0.f, 0.f, 0.f);
         renderer->UpdateBasicShaderMatrix(objectToRoom);
         for (const int p : model.nodes[i].primitives)
         {
            const Primitive& primitive = model.primitives[p];
            BaseTexture* const texture = (primitive.texture >= 0 && primitive.texture < static_cast<int>(model.textures.size())) ? model.textures[primitive.texture].get() : nullptr;
            m_rd->ResetRenderState();
            // The room space is mirrored compared to the glTF space, which reverses the winding of the triangles: both sides are drawn
            m_rd->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);
            shader->SetVector(ShaderUniform::fDisableLighting_top_below, controllerUnlitPart, 1.f, 0.f, 0.f);
            shader->SetVector(ShaderUniform::staticColor_Alpha, 1.f, 1.f, 1.f, 1.f);
            shader->SetAlphaTestValue(texture ? primitive.alphaTest : -1.f);
            if (texture)
               shader->SetTexture(ShaderUniform::tex_base_color, texture, false, SamplerFilter::SF_UNDEFINED, SamplerAddressMode::SA_REPEAT, SamplerAddressMode::SA_REPEAT);
            shader->SetMaterial(&primitive.material, primitive.transparent);
            shader->SetTechniqueMaterial(texture ? ShaderTechnique::basic_with_texture : ShaderTechnique::basic_without_texture, primitive.material, texture && primitive.alphaTest >= 0.f);
            if (primitive.transparent)
            {
               m_rd->EnableAlphaBlend(true);
               m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
            }
            else
               m_rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_TRUE);
            m_rd->DrawMesh(shader, primitive.transparent, center, 0.f, primitive.mesh, RenderDevice::TRIANGLELIST, 0, primitive.mesh->m_ib->m_count);
            drawn = true;
         }
      }
   }

   if (drawn)
   {
      renderer->UpdateBasicShaderMatrix();
      shader->SetVector(ShaderUniform::fDisableLighting_top_below, 0.f, 0.f, 0.f, 0.f);
   }
}

#endif
