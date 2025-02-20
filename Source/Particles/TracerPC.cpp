#include "TracerPC.H"

#include <AMReX_TracerParticle_mod_K.H>

using namespace amrex;

void
TracerPC::
InitParticles (const MultiFab& a_z_height)
{
    BL_PROFILE("TracerPC::InitParticles");

    const int lev = 0;
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        const Box& tile_box  = mfi.tilebox();
        const auto& height = a_z_height[mfi];
        const FArrayBox* height_ptr = nullptr;
#ifdef AMREX_USE_GPU
        std::unique_ptr<FArrayBox> hostfab;
        if (height.arena()->isManaged() || height.arena()->isDevice()) {
            hostfab = std::make_unique<FArrayBox>(height.box(), height.nComp(),
                                                  The_Pinned_Arena());
            Gpu::dtoh_memcpy_async(hostfab->dataPtr(), height.dataPtr(),
                                   height.size()*sizeof(Real));
            Gpu::streamSynchronize();
            height_ptr = hostfab.get();
        }
#else
        height_ptr = &height;
#endif
        Gpu::HostVector<ParticleType> host_particles;
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv)) {
            if (iv[0] == 3) {
                Real r[3] = {0.5, 0.5, 0.5};  // this means place at cell center

                Real x = plo[0] + (iv[0] + r[0])*dx[0];
                Real y = plo[1] + (iv[1] + r[1])*dx[1];
                Real z = (*height_ptr)(iv) + r[2]*((*height_ptr)(iv + IntVect(AMREX_D_DECL(0, 0, 1))) - (*height_ptr)(iv));

                ParticleType p;
                p.id()  = ParticleType::NextID();
                p.cpu() = ParallelDescriptor::MyProc();
                p.pos(0) = x;
                p.pos(1) = y;
                p.pos(2) = z;

                p.rdata(TracerRealIdx::old_x) = p.pos(0);
                p.rdata(TracerRealIdx::old_y) = p.pos(1);
                p.rdata(TracerRealIdx::old_z) = p.pos(2);

                p.idata(TracerIntIdx::k) = iv[2];  // particles carry their z-index

                host_particles.push_back(p);
           }
        }

        auto& particles = GetParticles(lev);
        auto& particle_tile = particles[std::make_pair(mfi.index(), mfi.LocalTileIndex())];
        auto old_size = particle_tile.GetArrayOfStructs().size();
        auto new_size = old_size + host_particles.size();
        particle_tile.resize(new_size);

        Gpu::copy(Gpu::hostToDevice,
                  host_particles.begin(),
                  host_particles.end(),
                  particle_tile.GetArrayOfStructs().begin() + old_size);
    }
}

/*
  /brief Uses midpoint method to advance particles using umac.
*/
void
TracerPC::AdvectWithUmac (Array<MultiFab const*, AMREX_SPACEDIM> umac,
                          int lev, Real dt, bool use_terrain, MultiFab& a_z_height)
{
    BL_PROFILE("TracerPC::AdvectWithUmac()");
    AMREX_ASSERT(OK(lev, lev, umac[0]->nGrow()-1));
    AMREX_ASSERT(lev >= 0 && lev < GetParticles().size());

    AMREX_D_TERM(AMREX_ASSERT(umac[0]->nGrow() >= 1);,
                 AMREX_ASSERT(umac[1]->nGrow() >= 1);,
                 AMREX_ASSERT(umac[2]->nGrow() >= 1););

    AMREX_D_TERM(AMREX_ASSERT(!umac[0]->contains_nan());,
                 AMREX_ASSERT(!umac[1]->contains_nan());,
                 AMREX_ASSERT(!umac[2]->contains_nan()););

    const auto      strttime = amrex::second();
    const Geometry& geom = m_gdb->Geom(lev);
    const Box& domain = geom.Domain();
    const auto plo = geom.ProbLoArray();
    const auto dxi = geom.InvCellSizeArray();
    const auto dx  = geom.CellSizeArray();

    Vector<std::unique_ptr<MultiFab> > raii_umac(AMREX_SPACEDIM);
    Vector<MultiFab const*> umac_pointer(AMREX_SPACEDIM);
    AMREX_ALWAYS_ASSERT(OnSameGrids(lev, *umac[0]));

     for (int i = 0; i < AMREX_SPACEDIM; i++) {
         umac_pointer[i] = umac[i];
     }

    for (int ipass = 0; ipass < 2; ipass++)
    {
#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for (ParIterType pti(*this, lev); pti.isValid(); ++pti)
        {
            int grid    = pti.index();
            auto& ptile = ParticlesAt(lev, pti);
            auto& aos  = ptile.GetArrayOfStructs();
            const int n = aos.numParticles();
            auto *p_pbox = aos().data();
            const FArrayBox* fab[AMREX_SPACEDIM] = { AMREX_D_DECL(&((*umac_pointer[0])[grid]),
                                                                  &((*umac_pointer[1])[grid]),
                                                                  &((*umac_pointer[2])[grid])) };

            //array of these pointers to pass to the GPU
            GpuArray<Array4<const Real>, AMREX_SPACEDIM>
                const umacarr {{AMREX_D_DECL((*fab[0]).array(),
                                             (*fab[1]).array(),
                                             (*fab[2]).array() )}};

            auto zheight      = use_terrain ? a_z_height[grid].array() : Array4<Real>{};

            ParallelFor(n,
                               [=] AMREX_GPU_DEVICE (int i)
            {
                ParticleType& p = p_pbox[i];
                if (p.id() <= 0) { return; }
                ParticleReal v[AMREX_SPACEDIM];
                mac_interpolate(p, plo, dxi, umacarr, v);
                if (ipass == 0)
                {
                    for (int dim=0; dim < AMREX_SPACEDIM; dim++)
                    {
                        p.rdata(dim) = p.pos(dim);
                        p.pos(dim) += static_cast<ParticleReal>(ParticleReal(0.5)*dt*v[dim]);
                    }
                }
                else
                {
                    for (int dim=0; dim < AMREX_SPACEDIM; dim++)
                    {
                        p.pos(dim) = p.rdata(dim) + static_cast<ParticleReal>(dt*v[dim]);
                        p.rdata(dim) = v[dim];
                    }

                    // also update z-coordinate here
                    IntVect iv(
                        AMREX_D_DECL(int(amrex::Math::floor((p.pos(0)-plo[0])*dxi[0])),
                                     int(amrex::Math::floor((p.pos(1)-plo[1])*dxi[1])),
                                     p.idata(0)));
                    iv[0] += domain.smallEnd()[0];
                    iv[1] += domain.smallEnd()[1];
                    ParticleReal zlo, zhi;
                    if (use_terrain) {
                        zlo = zheight(iv[0], iv[1], iv[2]);
                        zhi = zheight(iv[0], iv[1], iv[2]+1);
                    } else {
                        zlo =  iv[2]    * dx[2];
                        zhi = (iv[2]+1) * dx[2];
                    }
                    if (p.pos(2) > zhi) { // need to be careful here
                        p.idata(0) += 1;
                    } else if (p.pos(2) <= zlo) {
                        p.idata(0) -= 1;
                    }
                }
            });
        }
    }

    if (m_verbose > 1)
    {
        auto stoptime = amrex::second() - strttime;

#ifdef AMREX_LAZY
        Lazy::QueueReduction( [=] () mutable {
#endif
                ParallelReduce::Max(stoptime, ParallelContext::IOProcessorNumberSub(),
                                    ParallelContext::CommunicatorSub());

                amrex::Print() << "TracerParticleContainer::AdvectWithUmac() time: " << stoptime << '\n';
#ifdef AMREX_LAZY
        });
#endif
    }
}
