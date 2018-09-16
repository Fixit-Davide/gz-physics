/*
 * Copyright (C) 2018 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "SDFFeatures.hh"

#include <dart/constraint/ConstraintSolver.hpp>
#include <dart/dynamics/BallJoint.hpp>
#include <dart/dynamics/BoxShape.hpp>
#include <dart/dynamics/CylinderShape.hpp>
#include <dart/dynamics/FreeJoint.hpp>
#include <dart/dynamics/MeshShape.hpp>
#include <dart/dynamics/PlaneShape.hpp>
#include <dart/dynamics/PrismaticJoint.hpp>
#include <dart/dynamics/RevoluteJoint.hpp>
#include <dart/dynamics/ScrewJoint.hpp>
#include <dart/dynamics/SphereShape.hpp>
#include <dart/dynamics/UniversalJoint.hpp>
#include <dart/constraint/WeldJointConstraint.hpp>
#include <dart/dynamics/WeldJoint.hpp>

#include <cmath>

#include <ignition/math/eigen3/Conversions.hh>

#include <sdf/Box.hh>
#include <sdf/Collision.hh>
#include <sdf/Cylinder.hh>
#include <sdf/Geometry.hh>
#include <sdf/Joint.hh>
#include <sdf/JointAxis.hh>
#include <sdf/Link.hh>
#include <sdf/Material.hh>
#include <sdf/Mesh.hh>
#include <sdf/Model.hh>
#include <sdf/Sphere.hh>
#include <sdf/Visual.hh>
#include <sdf/World.hh>

namespace ignition {
namespace physics {
namespace dartsim {

namespace {
/////////////////////////////////////////////////
double infIfNeg(const double _value)
{
  if (_value < 0.0)
    return std::numeric_limits<double>::infinity();

  return _value;
}

/////////////////////////////////////////////////
template <typename Properties>
static void CopyStandardJointAxisProperties(
    const int _index, Properties &_properties,
    const ::sdf::JointAxis *_sdfAxis)
{
  _properties.mInitialPositions[_index] = _sdfAxis->InitialPosition();
  _properties.mDampingCoefficients[_index] = _sdfAxis->Damping();
  _properties.mFrictions[_index] = _sdfAxis->Friction();
  _properties.mRestPositions[_index] = _sdfAxis->SpringReference();
  _properties.mSpringStiffnesses[_index] = _sdfAxis->SpringStiffness();
  _properties.mPositionLowerLimits[_index] = _sdfAxis->Lower();
  _properties.mPositionUpperLimits[_index] = _sdfAxis->Upper();
  _properties.mForceLowerLimits[_index] = -infIfNeg(_sdfAxis->Effort());
  _properties.mForceUpperLimits[_index] =  infIfNeg(_sdfAxis->Effort());
  _properties.mVelocityLowerLimits[_index] = -infIfNeg(_sdfAxis->MaxVelocity());
  _properties.mVelocityUpperLimits[_index] =  infIfNeg(_sdfAxis->MaxVelocity());

  // TODO(MXG): Can dartsim support "Stiffness" and "Dissipation"?
}

/////////////////////////////////////////////////
static Eigen::Isometry3d GetParentModelFrame(
    const ModelInfo &_modelInfo)
{
  return _modelInfo.frame->getWorldTransform();
}

/////////////////////////////////////////////////
static Eigen::Vector3d ConvertJointAxis(
    const ::sdf::JointAxis *_sdfAxis,
    const ModelInfo &_modelInfo,
    const Eigen::Isometry3d &_T_joint)
{
  const Eigen::Vector3d axis = ignition::math::eigen3::convert(_sdfAxis->Xyz());

  if (_sdfAxis->UseParentModelFrame())
  {
    const Eigen::Quaterniond O_R_J{_T_joint.rotation()};
    const Eigen::Quaterniond O_R_M{GetParentModelFrame(_modelInfo).rotation()};
    const Eigen::Quaterniond J_R_M = O_R_J.inverse() * O_R_M;
    return J_R_M * axis;
  }

  return axis;
}

/////////////////////////////////////////////////
template <typename JointType>
static JointType *ConstructSingleAxisJoint(
    const ModelInfo &_modelInfo,
    const ::sdf::Joint &_sdfJoint,
    dart::dynamics::BodyNode * const _parent,
    dart::dynamics::BodyNode * const _child,
    const Eigen::Isometry3d &_T_joint)
{
  typename JointType::Properties properties;

  const ::sdf::JointAxis * const sdfAxis = _sdfJoint.Axis(0);
  properties.mAxis = ConvertJointAxis(sdfAxis, _modelInfo, _T_joint);

  CopyStandardJointAxisProperties(0, properties, sdfAxis);

  return _child->moveTo<JointType>(_parent, properties);
}

/////////////////////////////////////////////////
static dart::dynamics::UniversalJoint *ConstructUniversalJoint(
    const ModelInfo &_modelInfo,
    const ::sdf::Joint &_sdfJoint,
    dart::dynamics::BodyNode * const _parent,
    dart::dynamics::BodyNode * const _child,
    const Eigen::Isometry3d &_T_joint)
{
  dart::dynamics::UniversalJoint::Properties properties;

  for (const std::size_t index : {0u, 1u})
  {
    const ::sdf::JointAxis * const sdfAxis = _sdfJoint.Axis(index);
    properties.mAxis[index] = ConvertJointAxis(sdfAxis, _modelInfo, _T_joint);

    CopyStandardJointAxisProperties(index, properties, sdfAxis);
  }

  return _child->moveTo<dart::dynamics::UniversalJoint>(_parent, properties);
}

/////////////////////////////////////////////////
struct ShapeAndTransform
{
  dart::dynamics::ShapePtr shape;
  Eigen::Isometry3d tf = Eigen::Isometry3d::Identity();
};

/////////////////////////////////////////////////
static ShapeAndTransform ConstructBox(
    const ::sdf::Box &_box)
{
  return {std::make_shared<dart::dynamics::BoxShape>(
        math::eigen3::convert(_box.Size()))};
}

/////////////////////////////////////////////////
static ShapeAndTransform ConstructCylinder(
    const ::sdf::Cylinder &_cylinder)
{
  return {std::make_shared<dart::dynamics::CylinderShape>(
        _cylinder.Radius(), _cylinder.Length())};
}

/////////////////////////////////////////////////
static ShapeAndTransform ConstructSphere(
    const ::sdf::Sphere &_sphere)
{
  return {std::make_shared<dart::dynamics::SphereShape>(_sphere.Radius())};
}

/////////////////////////////////////////////////
static ShapeAndTransform ConstructPlane(
    const ::sdf::Plane &_plane)
{
  // TODO(MXG): We can consider using dart::dynamics::PlaneShape here, but that
  // would be an infinite plane, whereas we're supposed to produce a plane with
  // limited reach.
  //
  // So instead, we'll construct a very thin box with the requested length and
  // width, and transform it to point in the direction of the normal vector.
  const Eigen::Vector3d z = Eigen::Vector3d::UnitZ();
  const Eigen::Vector3d axis = z.cross(math::eigen3::convert(_plane.Normal()));
  const double norm = axis.norm();
  const double angle = std::asin(norm/(_plane.Normal().Length()));
  Eigen::Isometry3d R = Eigen::Isometry3d::Identity();

  // We check that the angle isn't too close to zero, because otherwise
  // axis/norm would be undefined.
  if (angle > 1e-12)
    R.rotate(Eigen::AngleAxisd(angle, axis/norm));

  return {std::make_shared<dart::dynamics::BoxShape>(
          Eigen::Vector3d(_plane.Size()[0], _plane.Size()[1], 1e-4)), R};
}

/////////////////////////////////////////////////
static ShapeAndTransform ConstructMesh(
    const ::sdf::Mesh & /*_mesh*/)
{
  // TODO(MXG): Look into what kind of mesh URI we get here. Will it just be
  // a local file name, or do we need to resolve the URI?
  std::cerr << "[dartsim::ConstructMesh] Mesh construction from an SDF has not "
            << "been implemented yet for dartsim.\n";
  return {nullptr};
}

/////////////////////////////////////////////////
static ShapeAndTransform ConstructGeometry(
    const ::sdf::Geometry &_geometry)
{
  if (_geometry.BoxShape())
    return ConstructBox(*_geometry.BoxShape());
  else if (_geometry.CylinderShape())
    return ConstructCylinder(*_geometry.CylinderShape());
  else if (_geometry.SphereShape())
    return ConstructSphere(*_geometry.SphereShape());
  else if (_geometry.PlaneShape())
    return ConstructPlane(*_geometry.PlaneShape());
  else if (_geometry.MeshShape())
    return ConstructMesh(*_geometry.MeshShape());

  return {nullptr};
}
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfWorld(
    const std::size_t /*_engine*/,
    const ::sdf::World &_sdfWorld)
{
  dart::simulation::WorldPtr world =
      std::make_shared<dart::simulation::World>();

  const std::size_t worldID = this->AddWorld(world, _sdfWorld.Name());

  world->setGravity(ignition::math::eigen3::convert(_sdfWorld.Gravity()));

  // TODO(MXG): Add a Physics class to the SDFormat DOM and then parse that
  // information here. For now, we'll just use dartsim's default physics
  // parameters.

  for (std::size_t i=0; i < _sdfWorld.ModelCount(); ++i)
  {
    const ::sdf::Model *model = _sdfWorld.ModelByIndex(i);

    if (!model)
      continue;

    this->ConstructSdfModel(worldID, *model);
  }

  return this->GenerateIdentity(worldID, world);
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfModel(
    const std::size_t _worldID,
    const ::sdf::Model &_sdfModel)
{
  dart::dynamics::SkeletonPtr model =
      dart::dynamics::Skeleton::create(_sdfModel.Name());

  dart::dynamics::SimpleFramePtr modelFrame =
      dart::dynamics::SimpleFrame::createShared(
        dart::dynamics::Frame::World(),
        _sdfModel.Name()+"_frame",
        math::eigen3::convert(_sdfModel.Pose()));

  auto [modelID, modelInfo] = this->AddModel({model, modelFrame}, _worldID); // NOLINT

  model->setMobile(!_sdfModel.Static());
  model->setSelfCollisionCheck(_sdfModel.SelfCollide());

  // First, construct all links
  for (std::size_t i=0; i < _sdfModel.LinkCount(); ++i)
  {
    this->FindOrConstructLink(
          model, modelID, _sdfModel, _sdfModel.LinkByIndex(i)->Name());
  }


  // Next, join all links that have joints
  for (std::size_t i=0; i < _sdfModel.JointCount(); ++i)
  {
    const ::sdf::Joint *sdfJoint = _sdfModel.JointByIndex(i);
    if (!sdfJoint)
    {
      std::cerr << "[dartsim::ConstructSdfModel] Error: The joint with "
                << "index [" << i << "] in model [" << _sdfModel.Name()
                << "] is a nullptr. It will be skipped.\n";
      continue;
    }

    dart::dynamics::BodyNode * const parent = this->FindOrConstructLink(
          model, modelID, _sdfModel, sdfJoint->ParentLinkName());

    dart::dynamics::BodyNode * const child = this->FindOrConstructLink(
          model, modelID, _sdfModel, sdfJoint->ChildLinkName());

    this->ConstructSdfJoint(modelInfo, *sdfJoint, parent, child);
  }

  return this->GenerateIdentity(modelID, model);
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfLink(
    const std::size_t _modelID,
    const ::sdf::Link &_sdfLink)
{
  const ModelInfo &modelInfo = models.at(_modelID);
  dart::dynamics::BodyNode::Properties bodyProperties;
  bodyProperties.mName = _sdfLink.Name();

  const ignition::math::Inertiald &sdfInertia = _sdfLink.Inertial();
  bodyProperties.mInertia.setMass(sdfInertia.MassMatrix().Mass());

  const Eigen::Matrix3d R_inertial{
        math::eigen3::convert(sdfInertia.Pose().Rot())};

  const Eigen::Matrix3d I_link =
      R_inertial
      * math::eigen3::convert(sdfInertia.Moi())
      * R_inertial.inverse();

  bodyProperties.mInertia.setMoment(I_link);

  const Eigen::Vector3d localCom =
      math::eigen3::convert(sdfInertia.Pose().Pos());

  bodyProperties.mInertia.setLocalCOM(localCom);

  dart::dynamics::FreeJoint::Properties jointProperties;
  jointProperties.mName = bodyProperties.mName + "_FreeJoint";
  // TODO(MXG): Consider adding a UUID to this joint name in order to avoid any
  // potential (albeit unlikely) name collisions.

  // Note: When constructing a link from this function, we always instantiate
  // it as a standalone free body within the model. If it should have any joint
  // constraints, those will be added later.
  const auto result = modelInfo.model->createJointAndBodyNodePair<
      dart::dynamics::FreeJoint>(nullptr, jointProperties, bodyProperties);

  dart::dynamics::FreeJoint * const joint = result.first;
  const Eigen::Isometry3d tf =
      this->ResolveSdfLinkReferenceFrame(_sdfLink.PoseFrame(), modelInfo)
      * math::eigen3::convert(_sdfLink.Pose());

  joint->setTransform(tf);

  dart::dynamics::BodyNode * const bn = result.second;

  const std::size_t linkID = this->AddLink(bn);
  this->AddJoint(joint);

  if (modelInfo.model->getNumBodyNodes() == 1)
  {
    // We just added the first link, so this is now the canonical link. We
    // should therefore move the "model frame" from the world onto this new
    // link, while preserving its location in the world frame.
    const dart::dynamics::SimpleFramePtr &modelFrame = modelInfo.frame;
    const Eigen::Isometry3d tf_frame = modelFrame->getWorldTransform();
    modelFrame->setParentFrame(bn);
    modelFrame->setTransform(tf_frame);
  }

  for (std::size_t i = 0; i < _sdfLink.CollisionCount(); ++i)
  {
    const auto collision = _sdfLink.CollisionByIndex(i);
    if (collision)
      this->ConstructSdfCollision(linkID, *collision);
  }

  for (std::size_t i = 0; i < _sdfLink.VisualCount(); ++i)
  {
    const auto visual = _sdfLink.VisualByIndex(i);
    if (visual)
      this->ConstructSdfVisual(linkID, *visual);
  }

  return this->GenerateIdentity(linkID);
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfJoint(
    const std::size_t _modelID,
    const ::sdf::Joint &_sdfJoint)
{
  const ModelInfo &modelInfo = models[_modelID];
  dart::dynamics::BodyNode * const parent =
      modelInfo.model->getBodyNode(_sdfJoint.ParentLinkName());

  dart::dynamics::BodyNode * const child =
      modelInfo.model->getBodyNode(_sdfJoint.ChildLinkName());

  return ConstructSdfJoint(modelInfo, _sdfJoint, parent, child);
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfCollision(
    const std::size_t _linkID,
    const ::sdf::Collision &_collision)
{
  if (!_collision.Geom())
  {
    std::cerr << "[dartsim::ConstructSdfCollision] Error: the geometry element "
              << "of collision [" << _collision.Name() << "] was a nullptr\n";
    return this->GenerateInvalidId();
  }

  const ShapeAndTransform st = ConstructGeometry(*_collision.Geom());
  const dart::dynamics::ShapePtr shape = st.shape;
  const Eigen::Isometry3d tf_shape = st.tf;

  if (!shape)
  {
    // The geometry element was empty, or the shape type is not supported
    return this->GenerateInvalidId();
  }

  dart::dynamics::BodyNode * const bn = this->links.at(_linkID);

  // NOTE(MXG): Gazebo requires unique collision shape names per Link, but
  // dartsim requires unique ShapeNode names per Skeleton, so we decorate the
  // Collision name for uniqueness sake.
  const std::string internalName =
      bn->getName() + "_collision_" + _collision.Name();

  dart::dynamics::ShapeNode * const node =
      bn->createShapeNodeWith<dart::dynamics::CollisionAspect>(
        shape, internalName);

  node->setRelativeTransform(
        math::eigen3::convert(_collision.Pose()) * tf_shape);

  return this->GenerateIdentity(this->AddShape({node, tf_shape}));
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfVisual(
    const std::size_t _linkID,
    const ::sdf::Visual &_visual)
{
  if (!_visual.Geom())
  {
    std::cerr << "[dartsim::ConstructSdfVisual] Error: the geometry element "
              << "of visual [" << _visual.Name() << "] was a nullptr\n";
    return this->GenerateInvalidId();
  }

  const ShapeAndTransform st = ConstructGeometry(*_visual.Geom());
  const dart::dynamics::ShapePtr shape = st.shape;
  const Eigen::Isometry3d tf_shape = st.tf;

  if (!shape)
  {
    // The geometry element was empty, or the shape type is not supported
    return this->GenerateInvalidId();
  }

  dart::dynamics::BodyNode * const bn = this->links.at(_linkID);

  // NOTE(MXG): Gazebo requires unique collision shape names per Link, but
  // dartsim requires unique ShapeNode names per Skeleton, so we decorate the
  // Collision name for uniqueness sake.
  const std::string internalName = bn->getName() + "_visual_" + _visual.Name();

  dart::dynamics::ShapeNode * const node =
      bn->createShapeNodeWith<dart::dynamics::VisualAspect>(
        shape, internalName);

  node->setRelativeTransform(
        math::eigen3::convert(_visual.Pose()) * tf_shape);

  // TODO(MXG): Are there any other visual parameters that we can do anything
  // with? Do these visual parameters even matter, since dartsim is only
  // intended for the physics?
  if (_visual.Material())
  {
    const ignition::math::Color &color = _visual.Material()->Ambient();
    node->getVisualAspect()->setColor(
          Eigen::Vector4d(color.R(), color.G(), color.B(), color.A()));
  }

  return this->GenerateIdentity(this->AddShape({node, tf_shape}));
}

/////////////////////////////////////////////////
dart::dynamics::BodyNode *SDFFeatures::FindOrConstructLink(
    const dart::dynamics::SkeletonPtr &_model,
    const std::size_t _modelID,
    const ::sdf::Model &_sdfModel,
    const std::string &_linkName)
{
  dart::dynamics::BodyNode * link = _model->getBodyNode(_linkName);
  if (link)
    return link;

  const ::sdf::Link * const sdfLink = _sdfModel.LinkByName(_linkName);
  if (!sdfLink)
  {
    std::cerr << "[dartsim::ConstructSdfModel] Error: Model ["
              << _sdfModel.Name() << "] does not contain a Link with the "
              << "name [" << _linkName << "].\n";
    return nullptr;
  }

  return this->links.at(this->ConstructSdfLink(_modelID, *sdfLink));
}

/////////////////////////////////////////////////
Identity SDFFeatures::ConstructSdfJoint(
    const ModelInfo &_modelInfo,
    const ::sdf::Joint &_sdfJoint,
    dart::dynamics::BodyNode * const _parent,
    dart::dynamics::BodyNode * const _child)
{
  if (!_parent || !_child)
  {
    std::stringstream msg;
    msg << "[dartsim::ConstructSdfJoint] Error: Asked to create a joint from "
        << "link [" << _sdfJoint.ParentLinkName() << "] to link ["
        << _sdfJoint.ChildLinkName() << "] in the model ["
        << _modelInfo.model->getName() << "], but ";

    if (!_parent)
    {
      msg << "the parent link ";
      if (!_child)
        msg << " and ";
    }

    if (!_child)
      msg << "the child link ";

    msg << "could not be found in that model!\n";
    std::cerr << msg.str();

    return this->GenerateInvalidId();
  }

  if (_parent->descendsFrom(_child))
  {
    // TODO(MXG): Add support for non-tree graph structures
    std::cerr << "[dartsim::ConstructSdfJoint] Error: Asked to create a "
              << "closed kinematic chain between links ["
              << _parent->getName() << "] and [" << _child->getName()
              << "], but that is not supported by the dartsim wrapper yet.\n";
    return this->GenerateInvalidId();
  }

  // Save the current transforms of the links so we remember it later
  const Eigen::Isometry3d T_parent = _parent->getWorldTransform();
  const Eigen::Isometry3d T_child = _child->getWorldTransform();

  const Eigen::Isometry3d T_joint =
    this->ResolveSdfJointReferenceFrame(_sdfJoint.PoseFrame(), _child)
    * math::eigen3::convert(_sdfJoint.Pose());

  const ::sdf::JointType type = _sdfJoint.Type();
  dart::dynamics::Joint *joint = nullptr;

  if (::sdf::JointType::BALL == type)
  {
    // SDF does not support any of the properties for ball joint, besides the
    // name and relative transforms to its parent and child, which will be taken
    // care of below. All other properties like joint limits, stiffness, etc,
    // will be the default values of +/- infinity or 0.0.
    joint = _child->moveTo<dart::dynamics::BallJoint>(_parent);
  }
  // TODO(MXG): Consider adding dartsim support for a CONTINUOUS joint type.
  // Alternatively, support the CONTINUOUS joint type by wrapping the
  // RevoluteJoint joint type.
  else if (::sdf::JointType::FIXED == type)
  {
    // A fixed joint does not have any properties besides the name and relative
    // transforms to its parent and child, which will be taken care of below.
    joint = _child->moveTo<dart::dynamics::WeldJoint>(_parent);
  }
  // TODO(MXG): Consider adding dartsim support for a GEARBOX joint type. It's
  // unclear to me whether it would be possible to get the same effect by
  // wrapping a RevoluteJoint type.
  else if (::sdf::JointType::PRISMATIC == type)
  {
    joint = ConstructSingleAxisJoint<dart::dynamics::PrismaticJoint>(
          _modelInfo, _sdfJoint, _parent, _child, T_joint);
  }
  else if (::sdf::JointType::REVOLUTE == type)
  {
    joint = ConstructSingleAxisJoint<dart::dynamics::RevoluteJoint>(
          _modelInfo, _sdfJoint, _parent, _child, T_joint);
  }
  // TODO(MXG): Consider adding dartsim support for a REVOLUTE2 joint type.
  // Alternatively, support the REVOLUTE2 joint type by wrapping two
  // RevoluteJoint objects into one.
  else if (::sdf::JointType::SCREW == type)
  {
    auto *screw = ConstructSingleAxisJoint<dart::dynamics::ScrewJoint>(
          _modelInfo, _sdfJoint, _parent, _child, T_joint);

    ::sdf::ElementPtr element = _sdfJoint.Element();
    if (element->HasElement("thread_pitch"))
    {
      screw->setPitch(element->GetElement("thread_pitch")->Get<double>());
    }

    joint = screw;
  }
  else if (::sdf::JointType::UNIVERSAL == type)
  {
    joint = ConstructUniversalJoint(
          _modelInfo, _sdfJoint, _parent, _child, T_joint);
  }
  else
  {
    std::cerr << "[dartsim::ConstructSdfJoint] Error: Asked to construct a "
              << "joint of sdf::JointType [" << static_cast<int>(type)
              << "], but that is not supported yet.\n";
    return this->GenerateInvalidId();
  }

  joint->setName(_sdfJoint.Name());

  // When initial positions are provided for joints, we need to correct the
  // parent transform:
  const Eigen::Isometry3d child_T_postjoint = T_child.inverse() * T_joint;
  const Eigen::Isometry3d parent_T_prejoint_init = T_parent.inverse() * T_joint;
  joint->setTransformFromParentBodyNode(parent_T_prejoint_init);
  joint->setTransformFromChildBodyNode(child_T_postjoint);

  // This is the transform inside the joint produced by whatever the current
  // joint position happens to be.
  const Eigen::Isometry3d prejoint_T_postjoint =
      parent_T_prejoint_init.inverse()
      * _child->getTransform(_parent)
      * child_T_postjoint;

  // This is the corrected transform needed to get the child link to its
  // correct pose (as specified by the loaded SDF) for the current initial
  // position
  const Eigen::Isometry3d parent_T_prejoint_final =
      _parent->getWorldTransform().inverse()
      * T_child
      * child_T_postjoint
      * prejoint_T_postjoint.inverse();

  joint->setTransformFromParentBodyNode(parent_T_prejoint_final);

  const std::size_t jointID = this->AddJoint(joint);

  return this->GenerateIdentity(jointID);
}

/////////////////////////////////////////////////
Eigen::Isometry3d SDFFeatures::ResolveSdfLinkReferenceFrame(
    const std::string &_frame,
    const ModelInfo &_modelInfo) const
{
  if (_frame.empty())
    return GetParentModelFrame(_modelInfo);

  std::cerr << "[dartsim::ResolveSdfLinkReferenceFrame] Requested a reference "
            << "frame of [" << _frame << "] but currently only the model frame "
            << "is supported as a reference frame for link poses.\n";

  // TODO(MXG): Implement this when frame specifications are nailed down
  return Eigen::Isometry3d::Identity();
}

/////////////////////////////////////////////////
Eigen::Isometry3d SDFFeatures::ResolveSdfJointReferenceFrame(
    const std::string &_frame,
    const dart::dynamics::BodyNode *_child) const
{
  if (_frame.empty())
  {
    // This means the joint pose is expressed relative to the child link pose
    return _child->getWorldTransform();
  }

  std::cerr << "[dartsim::ResolveSdfJointReferenceFrame] Requested a reference "
            << "frame of [" << _frame << "] but currently only the child link "
            << "frame is supported as a reference frame for joint poses.\n";

  // TODO(MXG): Implement this when frame specifications are nailed down
  return Eigen::Isometry3d::Identity();
}

}
}
}