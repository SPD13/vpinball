// license:GPLv3+

#pragma once

#if defined(ENABLE_XR)

#include "parts/Material.h"

class BaseTexture;
class MeshBuffer;
class Renderer;
class RenderDevice;
class VRDevice;

// Draws the controller models given by the OpenXR runtime (see VRDevice::GetControllerModels): their glTF assets are turned into meshes once,
// then each frame every node is placed with the pose of the model and the poses of its animated parts (buttons, triggers, thumbsticks).
class VRControllerModels final
{
public:
   explicit VRControllerModels(RenderDevice* rd);
   ~VRControllerModels();

   // Called while preparing a frame, with the room space reference selected (the models are placed in the real world)
   void Render(Renderer* renderer, const VRDevice& vrDevice);

private:
   struct Primitive
   {
      std::shared_ptr<MeshBuffer> mesh;
      int texture = -1; // Index in Model::textures
      Material material;
      float alphaTest = -1.f; // glTF alpha mask cutoff, or -1 without
      bool transparent = false; // glTF alpha blend
      bool sticker = false; // Front sticker of the controller, which shows our logo (see Load)
   };
   struct Node
   {
      int parent = -1; // Nodes are sorted parents first
      Matrix3D local; // glTF node transform (row vectors)
      vec3 scale { 1.f, 1.f, 1.f }; // Kept when the runtime gives the pose of the node
      int animatable = -1; // Index in the node states of the runtime, or -1 if not animated
      vector<int> primitives;
   };
   struct Model
   {
      uint64_t id = 0;
      vector<Node> nodes;
      vector<Primitive> primitives;
      vector<std::shared_ptr<BaseTexture>> textures;
   };
   std::unique_ptr<Model> Load(uint64_t id, const vector<uint8_t>& glb, const vector<string>& animatableNodes) const;
   void Release(Model& model) const;

   RenderDevice* const m_rd;
   vector<std::unique_ptr<Model>> m_models;
   std::shared_ptr<BaseTexture> m_stickerTexture; // Our logo, for the front sticker of the controllers
   bool m_stickerTextureLoaded = false;
   vector<uint64_t> m_failedIds; // Assets that could not be loaded, not tried again
   vector<Matrix3D> m_nodeWorld; // Scratch buffers for Render
   vector<bool> m_nodeVisible;
   double m_nextLogTime = 0.;
};

#endif
