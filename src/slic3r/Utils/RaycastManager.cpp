#include "RaycastManager.hpp"
#include <utility>

// include for earn camera
#include "slic3r/GUI/GUI_App.hpp" 
#include "slic3r/GUI/Plater.hpp"  
#include "slic3r/GUI/Camera.hpp"

using namespace Slic3r::GUI;

namespace priv {

using namespace Slic3r;
// copied from private part of RaycastManager.hpp
using Raycaster = std::pair<size_t, std::unique_ptr<MeshRaycaster> >;
//                      ModelVolume.id

using Raycasters = std::vector<Raycaster>;

static void actualize(Raycasters &casters, const ModelVolumePtrs &volumes, const RaycastManager::ISkip *skip)
{
    // check if volume was removed
    std::vector<bool> removed_casters(casters.size(), {true});
    // actualize MeshRaycaster
    for (const ModelVolume *volume : volumes) {
        size_t oid = volume->id().id;
        if (skip != nullptr && skip->skip(oid))
            continue;
        auto item = std::find_if(casters.begin(), casters.end(),
                                 [oid](const Raycaster &it) -> bool { return oid == it.first; });
        if (item == casters.end()) {
            // add new raycaster
            auto raycaster = std::make_unique<MeshRaycaster>(volume->get_mesh_shared_ptr());
            casters.emplace_back(std::make_pair(oid, std::move(raycaster)));
        } else {
            size_t index           = item - casters.begin();
            removed_casters[index] = false;
        }
    }
    
    // clean other raycasters
    for (int i = removed_casters.size() - 1; i >= 0; --i)
        if (removed_casters[i])
            casters.erase(casters.begin() + i);
}
}

void RaycastManager::actualize(const ModelObject *object, const ISkip *skip)
{
    // actualize MeshRaycaster
    priv::actualize(m_raycasters, object->volumes, skip);

    // check if inscance was removed
    std::vector<bool> removed_transf(m_transformations.size(), {true});
    
    // actualize transformation matrices
    for (const ModelVolume *volume : object->volumes) {
        if (skip != nullptr && skip->skip(volume->id().id)) continue;
        const Transform3d &volume_tr = volume->get_matrix();
        for (const ModelInstance *instance : object->instances) {
            const Transform3d &instrance_tr   = instance->get_matrix();
            Transform3d        transformation = instrance_tr * volume_tr;
            TrKey tr_key = std::make_pair(instance->id().id, volume->id().id);
            auto  item   = std::find_if(m_transformations.begin(),
                                        m_transformations.end(),
                                        [&tr_key](const TrItem &it) -> bool {
                                         return it.first == tr_key;
                                        });
            if (item != m_transformations.end()) {
                // actualize transformation all the time
                item->second = transformation;
                size_t index = item - m_transformations.begin();
                removed_transf[index] = false;
            } else {
                // add new transformation
                m_transformations.emplace_back(
                    std::make_pair(tr_key, transformation));
            }
        }
    }

    // clean other transformation
    for (int i = removed_transf.size() - 1; i >= 0; --i)
        if (removed_transf[i])
            m_transformations.erase(m_transformations.begin() + i);
}

void RaycastManager::actualize(const ModelInstance *instance, const ISkip *skip) {
    const ModelVolumePtrs &volumes = instance->get_object()->volumes;

    // actualize MeshRaycaster
    priv::actualize(m_raycasters, volumes, skip);

    // check if inscance was removed
    std::vector<bool> removed_transf(m_transformations.size(), {true});

    // actualize transformation matrices
    for (const ModelVolume *volume : volumes) {
        if (skip != nullptr && skip->skip(volume->id().id))
            continue;
        const Transform3d &volume_tr = volume->get_matrix();
        const Transform3d &instrance_tr   = instance->get_matrix();
        Transform3d        transformation = instrance_tr * volume_tr;
        TrKey tr_key = std::make_pair(instance->id().id, volume->id().id);
        auto  item   = std::find_if(m_transformations.begin(), m_transformations.end(),
                                    [&tr_key](const TrItem &it) -> bool { return it.first == tr_key; });
        if (item != m_transformations.end()) {
            // actualize transformation all the time
            item->second          = transformation;
            size_t index          = item - m_transformations.begin();
            removed_transf[index] = false;
        } else {
            // add new transformation
            m_transformations.emplace_back(std::make_pair(tr_key, transformation));
        }        
    }

    // clean other transformation
    for (int i = removed_transf.size() - 1; i >= 0; --i)
        if (removed_transf[i])
            m_transformations.erase(m_transformations.begin() + i);
}

#include "slic3r/GUI/CameraUtils.hpp"
namespace priv {

// Copy functionality from MeshRaycaster::unproject_on_mesh without filtering
static std::optional<RaycastManager::SurfacePoint> unproject_on_mesh(const MeshRaycaster &raycaster,
    const Vec2d &mouse_pos, const Transform3d &transformation, const Camera &camera) {

    Vec3d point;
    Vec3d direction;
    CameraUtils::ray_from_screen_pos(camera, mouse_pos, point, direction);
    Transform3d inv = transformation.inverse();
    point     = inv*point;
    direction = inv.linear()*direction;

    const AABBMesh &aabb_mesh = raycaster.get_aabb_mesh();
    std::vector<AABBMesh::hit_result> hits = aabb_mesh.query_ray_hits(point, direction);

    if (hits.empty())
        return {}; // no intersection found

    const AABBMesh::hit_result &hit = hits.front();
    return RaycastManager::SurfacePoint(
        hit.position().cast<float>(), 
        hit.normal().cast<float>()
    );
}
} // namespace priv

std::optional<RaycastManager::Hit> RaycastManager::unproject(
    const Vec2d &mouse_pos, const Camera &camera, const ISkip *skip) const
{
    std::optional<Hit> closest;
    for (const auto &item : m_transformations) { 
        const TrKey &key = item.first;
        size_t       volume_id = key.second;
        if (skip != nullptr && skip->skip(volume_id)) continue;
        const Transform3d &transformation = item.second;
        auto raycaster_it =
            std::find_if(m_raycasters.begin(), m_raycasters.end(),
                         [volume_id](const RaycastManager::Raycaster &it)
                             -> bool { return volume_id == it.first; });
        if (raycaster_it == m_raycasters.end()) continue;
        const MeshRaycaster &raycaster = *(raycaster_it->second);
        std::optional<SurfacePoint> surface_point_opt = priv::unproject_on_mesh(
            raycaster, mouse_pos, transformation, camera);
        if (!surface_point_opt.has_value())
            continue;
        const SurfacePoint &surface_point = *surface_point_opt;
        Vec3d act_hit_tr = transformation * surface_point.position.cast<double>();
        double squared_distance = (camera.get_position() - act_hit_tr).squaredNorm();
        if (closest.has_value() &&
            closest->squared_distance < squared_distance)
            continue;
        closest = Hit(key, surface_point, squared_distance);
    }

    //if (!closest.has_value()) return {};
    return closest;
}

std::optional<RaycastManager::Hit> RaycastManager::unproject(const Vec3d &point, const Vec3d &direction, const ISkip *skip) const
{
    std::optional<Hit> closest;
    for (const auto &item : m_transformations) { 
        const TrKey &key = item.first;
        size_t       volume_id = key.second;
        if (skip != nullptr && skip->skip(volume_id)) continue;
        const Transform3d &transformation = item.second;
        auto raycaster_it =
            std::find_if(m_raycasters.begin(), m_raycasters.end(),
                         [volume_id](const RaycastManager::Raycaster &it)
                             -> bool { return volume_id == it.first; });
        if (raycaster_it == m_raycasters.end()) continue;
        const MeshRaycaster &raycaster = *(raycaster_it->second);
        const AABBMesh& mesh = raycaster.get_aabb_mesh();                
        Transform3d tr_inv = transformation.inverse();
        Vec3d mesh_point = tr_inv * point;
        Vec3d mesh_direction = tr_inv.linear() * direction;

        // Need for detect that actual point position is on correct place
        Vec3d point_positive = mesh_point - mesh_direction;
        Vec3d point_negative = mesh_point + mesh_direction;

        // Throw ray to both directions of ray
        std::vector<AABBMesh::hit_result> hits = mesh.query_ray_hits(point_positive, mesh_direction);
        std::vector<AABBMesh::hit_result> hits_neg = mesh.query_ray_hits(point_negative, -mesh_direction);
        hits.insert(hits.end(), std::make_move_iterator(hits_neg.begin()), std::make_move_iterator(hits_neg.end()));
        for (const AABBMesh::hit_result &hit : hits) { 
            double squared_distance = (mesh_point - hit.position()).squaredNorm();
            if (closest.has_value() &&
                closest->squared_distance < squared_distance)
                continue;
            SurfacePoint surface_point(hit.position().cast<float>(), hit.normal().cast<float>());
            closest = Hit(key, surface_point, squared_distance);
        }
    }
    return closest;
}

std::optional<RaycastManager::Hit> RaycastManager::closest(const Vec3d &point, const ISkip *skip) const {
    std::optional<Hit> closest;
    for (const auto &item : m_transformations) {
        const TrKey &key       = item.first;
        size_t       volume_id = key.second;
        if (skip != nullptr && skip->skip(volume_id))
            continue;
        auto raycaster_it = std::find_if(m_raycasters.begin(), m_raycasters.end(), 
            [volume_id](const RaycastManager::Raycaster &it) -> bool { return volume_id == it.first; });
        if (raycaster_it == m_raycasters.end())
            continue;
        const MeshRaycaster &raycaster = *(raycaster_it->second);
        const Transform3d &transformation = item.second;
        Transform3d tr_inv = transformation.inverse();
        Vec3d mesh_point_d = tr_inv * point;
        Vec3f mesh_point_f = mesh_point_d.cast<float>();
        Vec3f n;
        Vec3f p = raycaster.get_closest_point(mesh_point_f, &n);
        double squared_distance = (mesh_point_f - p).squaredNorm();
        if (closest.has_value() && closest->squared_distance < squared_distance)
            continue;
        SurfacePoint surface_point(p,n);
        closest = Hit(key, surface_point, squared_distance);
    }
    return closest;
}

Slic3r::Transform3d RaycastManager::get_transformation(const TrKey &tr_key) const {
    auto item = std::find_if(m_transformations.begin(),
                             m_transformations.end(),
                             [&tr_key](const TrItem &it) -> bool {
                                 return it.first == tr_key;
                             });
    if (item == m_transformations.end()) return Transform3d::Identity();
    return item->second;
}