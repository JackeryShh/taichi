/*******************************************************************************
    Taichi - Physically based Computer Graphics Library

    Copyright (c) 2016 Yuanming Hu <yuanmhu@gmail.com>
                  2017 Yu Fang <squarefk@gmail.com>

    All rights reserved. Use of this source code is governed by
    the MIT license as written in the LICENSE file.
*******************************************************************************/

#include "mpm3.h"
#include <taichi/math/qr_svd.h>
#include <taichi/system/threading.h>
#include <taichi/visual/texture.h>
#include <taichi/math/math_util.h>
#include <taichi/common/asset_manager.h>

TC_NAMESPACE_BEGIN

// Note: assuming abs(x) <= 2!!
inline float w(float x) {
    x = abs(x);
    assert(x <= 2);
    if (x < 1) {
        return 0.5f * x * x * x - x * x + 2.0f / 3.0f;
    } else {
        return -1.0f / 6.0f * x * x * x + x * x - 2 * x + 4.0f / 3.0f;
    }
}

// Note: assuming abs(x) <= 2!!
inline float dw(float x) {
    float s = x < 0.0f ? -1.0f : 1.0f;
    x *= s;
    assert(x <= 2.0f);
    float val;
    float xx = x * x;
    if (x < 1.0f) {
        val = 1.5f * xx - 2.0f * x;
    } else {
        val = -0.5f * xx + 2.0f * x - 2.0f;
    }
    return s * val;
}

inline float w(const Vector3 &a) {
    return w(a.x) * w(a.y) * w(a.z);
}

inline Vector3 dw(const Vector3 &a) {
    return Vector3(dw(a.x) * w(a.y) * w(a.z), w(a.x) * dw(a.y) * w(a.z), w(a.x) * w(a.y) * dw(a.z));
}

void MPM3D::initialize(const Config &config) {
    Simulation3D::initialize(config);
    res = config.get_vec3i("resolution");
    gravity = config.get_vec3("gravity");
    apic = config.get("apic", true);
    async = config.get("async", false);
    base_delta_t = config.get("base_delta_t", 1e-6f);
    cfl = config.get("cfl", 1.0f);
    strength_dt_mul = config.get("strength_dt_mul", 1.0f);
    TC_LOAD_CONFIG(affine_damping, 0.0f);
    if (async) {
        maximum_delta_t = config.get("maximum_delta_t", 1e-1f);
    } else {
        maximum_delta_t = base_delta_t;
    }

    grid_velocity.initialize(res + Vector3i(1), Vector(0.0f), Vector3(0.0f));
    grid_mass.initialize(res + Vector3i(1), 0, Vector3(0.0f));
    grid_locks.initialize(res + Vector3i(1), 0, Vector3(0.0f));
    scheduler.initialize(res, base_delta_t, cfl, strength_dt_mul, &levelset);
}

void MPM3D::add_particles(const Config &config) {
    std::shared_ptr<Texture> density_texture = AssetManager::get_asset<Texture>(config.get_int("density_tex"));
    for (int i = 0; i < res[0]; i++) {
        for (int j = 0; j < res[1]; j++) {
            for (int k = 0; k < res[2]; k++) {
                Vector3 coord = Vector3(i + 0.5f, j + 0.5f, k + 0.5f) / Vector3(res);
                real num = density_texture->sample(coord).x;
                int t = (int)num + (rand() < num - int(num));
                for (int l = 0; l < t; l++) {
                    MPM3Particle *p = nullptr;
                    if (config.get("type", std::string("ep")) == std::string("ep")) {
                        p = new EPParticle3();
                        p->initialize(config);
                    } else {
                        p = new DPParticle3();
                        p->initialize(config);
                    }
                    p->pos = Vector(i + rand(), j + rand(), k + rand());
                    p->mass = 1.0f;
                    p->v = config.get("initial_velocity", p->v);
                    p->last_update = current_t_int;
                    particles.push_back(p);
                    scheduler.insert_particle(p, true);
                }
            }
        }
    }
    P(particles.size());
}

std::vector<RenderParticle> MPM3D::get_render_particles() const {
    using Particle = RenderParticle;
    std::vector<Particle> render_particles;
    render_particles.reserve(particles.size());
    Vector3 center(res[0] / 2.0f, res[1] / 2.0f, res[2] / 2.0f);
    for (auto p_p : particles) {
        MPM3Particle &p = *p_p;
        // at least synchronize the position
        Vector3 pos = p.pos - center + (current_t_int - p.last_update) * base_delta_t * p.v;
        if (p.state == MPM3Particle::UPDATING) {
            render_particles.push_back(Particle(pos, Vector4(0.8f, 0.1f, 0.2f, 0.5f)));
        } else
        if (p.state == MPM3Particle::BUFFER) {
            render_particles.push_back(Particle(pos, Vector4(0.8f, 0.8f, 0.2f, 0.5f)));
        }
        else {
            render_particles.push_back(Particle(pos, Vector4(0.8f, 0.9f, 1.0f, 0.5f)));
        }
    }
    return render_particles;
}

void MPM3D::rasterize() {
    grid_velocity.reset(Vector(0.0f));
    grid_mass.reset(0.0f);
    parallel_for_each_active_particle([&](MPM3Particle &p) {
        for (auto &ind : get_bounded_rasterization_region(p.pos)) {
            Vector3 d_pos = Vector(ind.i, ind.j, ind.k) - p.pos;
            real weight = w(d_pos);
            grid_locks[ind].lock();
            grid_mass[ind] += weight * p.mass;
            grid_velocity[ind] += weight * p.mass * (p.v + (3.0f) * p.apic_b * d_pos);
            grid_locks[ind].unlock();
        }
    });
    for (auto ind : grid_mass.get_region()) {
        if (grid_mass[ind] > 0) {
            CV(grid_velocity[ind]);
            CV(1 / grid_mass[ind]);
            grid_velocity[ind] = grid_velocity[ind] * (1.0f / grid_mass[ind]);
            CV(grid_velocity[ind]);
        }
    }
}

void MPM3D::resample() {
    real alpha_delta_t = 1;
    if (apic)
        alpha_delta_t = 0;
    parallel_for_each_active_particle([&](MPM3Particle &p) {
        if (p.state != MPM3Particle::UPDATING)
            return;
        real delta_t = base_delta_t * (current_t_int - p.last_update);
        Vector v(0.0f), bv(0.0f);
        Matrix cdg(0.0f);
        Matrix b(0.0f);
        int count = 0;
        for (auto &ind : get_bounded_rasterization_region(p.pos)) {
            count++;
            Vector d_pos = p.pos - Vector3(ind.i, ind.j, ind.k);
            float weight = w(d_pos);
            Vector gw = dw(d_pos);
            Vector grid_vel = grid_velocity[ind];
            v += weight * grid_vel;
            Vector aa = grid_vel;
            Vector bb = -d_pos;
            Matrix out(aa[0] * bb[0], aa[1] * bb[0], aa[2] * bb[0],
                       aa[0] * bb[1], aa[1] * bb[1], aa[2] * bb[1],
                       aa[0] * bb[2], aa[1] * bb[2], aa[2] * bb[2]);
            b += weight * out;
            bv += weight * grid_velocity_backup[ind];
            cdg += glm::outerProduct(grid_velocity[ind], gw);
            CV(grid_velocity[ind]);
        }
        if (count != 64 || !apic) {
            b = Matrix(0);
        }
        // We should use an std::exp here, but it is too slow...
        real damping = std::max(0.0f, 1.0f - delta_t * affine_damping);
        p.apic_b = b * damping;
        cdg = Matrix(1) + delta_t * cdg;
        p.v = (1 - alpha_delta_t) * v + alpha_delta_t * (v - bv + p.v);
        Matrix dg = cdg * p.dg_e * p.dg_p;
        p.dg_e = cdg * p.dg_e;
        p.dg_cache = dg;
    });
}

void MPM3D::apply_deformation_force(float delta_t) {
    //printf("Calculating force...\n");
    parallel_for_each_active_particle([&](MPM3Particle &p) {
        p.calculate_force();
    });
    //printf("Accumulating force...\n");
    parallel_for_each_active_particle([&](MPM3Particle &p) {
        for (auto &ind : get_bounded_rasterization_region(p.pos)) {
            real mass = grid_mass[ind];
            if (mass == 0.0f) { // No EPS here
                continue;
            }
            Vector d_pos = p.pos - Vector3(ind.i, ind.j, ind.k);
            Vector gw = dw(d_pos);
            Vector force = p.tmp_force * gw;
            CV(force);
            grid_locks[ind].lock();
            grid_velocity[ind] += delta_t / mass * force;
            grid_locks[ind].unlock();
        }
    });
}

void MPM3D::grid_apply_boundary_conditions(const DynamicLevelSet3D &levelset, real t) {
    for (auto &ind : scheduler.get_active_grid_points()) {
        Vector3 pos = Vector3(0.5 + ind[0], 0.5 + ind[1], 0.5 + ind[2]);
        real phi = levelset.sample(pos, t);
        if (1 < phi || phi < -3) continue;
        Vector3 n = levelset.get_spatial_gradient(pos, t);
        Vector boundary_velocity = levelset.get_temporal_derivative(pos, t) * n;
        Vector3 v = grid_velocity[ind] - boundary_velocity;
        if (phi > 0) { // 0~1
            real pressure = std::max(-glm::dot(v, n), 0.0f);
            real mu = levelset.levelset0->friction;
            if (mu < 0) { // sticky
                v = Vector3(0.0f);
            } else {
                Vector3 t = v - n * glm::dot(v, n);
                if (length(t) > 1e-6f) {
                    t = normalize(t);
                }
                real friction = -clamp(glm::dot(t, v), -mu * pressure, mu * pressure);
                v = v + n * pressure + t * friction;
            }
        } else if (phi < 0.0f) {
            v = n * std::max(0.0f, glm::dot(v, n));
        }
        v += boundary_velocity;
        grid_velocity[ind] = v;
    }
}

void MPM3D::particle_collision_resolution(real t) {
    parallel_for_each_active_particle([&](MPM3Particle &p) {
        if (p.state == MPM3Particle::UPDATING) {
            p.resolve_collision(levelset, t);
        }
    });
}

void MPM3D::substep() {
    if (!particles.empty()) {
        /*
        for (auto &p : particles) {
            p.calculate_kernels();
        }
        */

        scheduler.update_particle_groups();
        scheduler.reset_particle_states();
//        int64 original_t_int_increment;
//        int64 t_int_increment;
        old_t_int = current_t_int;
        if (async) {
            scheduler.reset();
            scheduler.update_dt_limits(current_t);

            original_t_int_increment = std::min(get_largest_pot(int64(maximum_delta_t / base_delta_t)),
                                                scheduler.update_max_dt_int(current_t_int));

            t_int_increment = original_t_int_increment - current_t_int % original_t_int_increment;

            current_t_int += t_int_increment;
            current_t = current_t_int * base_delta_t;

            scheduler.set_time(current_t_int);

            scheduler.expand(false, true);
        } else {
            // sync
            t_int_increment = 1;
            scheduler.states = 2;
            for (auto &p : particles) {
                p->state = MPM3Particle::UPDATING;
            }
            current_t_int += t_int_increment;
            current_t = current_t_int * base_delta_t;
        }
        scheduler.update();
//        P(scheduler.active_grid_points.size());
//        P(scheduler.active_particles.size());

        rasterize();
        grid_backup_velocity();
        grid_apply_external_force(gravity, t_int_increment * base_delta_t);
        apply_deformation_force(t_int_increment * base_delta_t);
        grid_apply_boundary_conditions(levelset, current_t);
        resample();
        parallel_for_each_particle([&](MPM3Particle &p) {
            if (p.state == MPM3Particle::UPDATING) {
                p.pos += (current_t_int - p.last_update) * base_delta_t * p.v;
                p.last_update = current_t_int;
                p.pos.x = clamp(p.pos.x, 0.0f, res[0] - eps);
                p.pos.y = clamp(p.pos.y, 0.0f, res[1] - eps);
                p.pos.z = clamp(p.pos.z, 0.0f, res[2] - eps);
                p.plasticity();
            }
        });
        particle_collision_resolution(current_t);
        if (async) {
            scheduler.enforce_smoothness(original_t_int_increment);
        }
    }
}

bool MPM3D::test() const {
    for (int i = 0; i < 100000; i++) {
        Matrix3 m(1.000000238418579101562500000000, -0.000000000000000000000000000000,
                  -0.000000000000000000000220735070, 0.000000000000000000000000000000, 1.000000238418579101562500000000,
                  -0.000000000000000000216840434497, 0.000000000000000000000211758237,
                  -0.000000000000000001084202172486, 1.000000000000000000000000000000);
        Matrix3 u, sig, v;
        svd(m, u, sig, v);
        if (!is_normal(sig)) {
            P(m);
            P(u);
            P(sig);
            P(v);
        }
    }
    return false;
}

TC_IMPLEMENTATION(Simulation3D, MPM3D, "mpm");

TC_NAMESPACE_END
