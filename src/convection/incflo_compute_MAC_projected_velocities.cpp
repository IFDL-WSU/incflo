#include <hydro_mol.H>
#include <hydro_godunov.H>
#include <incflo.H>

#ifdef AMREX_USE_EB
#include <hydro_ebmol.H>
#include <hydro_ebgodunov.H>
#endif

#include <AMReX_MultiFabUtil.H>
#include <AMReX_MacProjector.H>

using namespace amrex;

void
incflo::compute_MAC_projected_velocities (
                                 Vector<MultiFab const*> const& vel,
                                 Vector<MultiFab const*> const& density,
                                 AMREX_D_DECL(Vector<MultiFab*> const& u_mac,
                                              Vector<MultiFab*> const& v_mac,
                                              Vector<MultiFab*> const& w_mac),
                                 Vector<MultiFab*> const& vel_forces,
                                 Real /*time*/)
{
    BL_PROFILE("incflo::compute_MAC_projected_velocities()");
    Real l_dt = m_dt;

    auto mac_phi = get_mac_phi();

    // We first compute the velocity forcing terms to be used in predicting
    //    to faces before the MAC projection
    if (m_advection_type != "MOL") {

        // bool include_pressure_gradient = !(m_use_mac_phi_in_godunov);
        // compute_vel_forces(vel_forces, vel, density, tracer, tracer, include_pressure_gradient);

        if (m_godunov_include_diff_in_forcing)
            for (int lev = 0; lev <= finest_level; ++lev)
                MultiFab::Add(*vel_forces[lev], m_leveldata[lev]->divtau_o, 0, 0, AMREX_SPACEDIM, 0);

        if (nghost_force() > 0)
            fillpatch_force(m_cur_time, vel_forces, nghost_force());
    }


    // This will hold (1/rho) on faces
    Vector<MultiFab*> inv_rho_x(finest_level+1);
    Vector<MultiFab*> inv_rho_y(finest_level+1);
#if (AMREX_SPACEDIM == 3)
    Vector<MultiFab*> inv_rho_z(finest_level+1);
#endif

    for (int lev = 0; lev <= finest_level; ++lev) {

        AMREX_D_TERM(
           inv_rho_x[lev]->define(u_mac[lev]->boxArray(),dmap[lev],1,0,MFInfo(),Factory(lev));,
           inv_rho_y[lev]->define(v_mac[lev]->boxArray(),dmap[lev],1,0,MFInfo(),Factory(lev));,
           inv_rho_z[lev]->define(w_mac[lev]->boxArray(),dmap[lev],1,0,MFInfo(),Factory(lev)););
    }

    Vector<Array<MultiFab*,AMREX_SPACEDIM> > inv_rho(finest_level+1);

    for (int lev=0; lev <= finest_level; ++lev)
    {
        AMREX_D_TERM(inv_rho[lev][0] = inv_rho_x[lev];,
                     inv_rho[lev][1] = inv_rho_y[lev];,
                     inv_rho[lev][2] = inv_rho_z[lev];);

#ifdef AMREX_USE_EB
        EB_interp_CellCentroid_to_FaceCentroid (*density[lev], inv_rho[lev],
                                                0, 0, 1, geom[lev], get_density_bcrec());
#else
        amrex::average_cellcenter_to_face(inv_rho[lev], *density[lev], geom[lev]);
#endif

        for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) {
            inv_rho[lev][idim]->invert(1.0, 0);
        }
    }

    //
    // Initialize (or redefine the beta in) the MacProjector
    //
    if (macproj->needInitialization()) 
    {
        LPInfo lp_info;
        lp_info.setMaxCoarseningLevel(m_mac_mg_max_coarsening_level);
#ifndef AMREX_USE_EB
        if (m_constant_density) {
            Vector<BoxArray> ba;
            Vector<DistributionMapping> dm;
            for (auto const& ir : inv_rho) {
                ba.push_back(ir[0]->boxArray());
                dm.push_back(ir[0]->DistributionMap());
            }
            macproj->initProjector(ba, dm, lp_info, 1.0/m_ro_0);
        } else
#endif
        {
            macproj->initProjector(lp_info, inv_rho);
        }
        macproj->setDomainBC(get_projection_bc(Orientation::low), get_projection_bc(Orientation::high));
    } else {
#ifndef AMREX_USE_EB
        if (m_constant_density) {
            macproj->updateBeta(1.0/m_ro_0);  // unnecessary unless m_ro_0 changes.
        } else
#endif
        {
            macproj->updateBeta(inv_rho);
        }
    }

    Vector<Array<MultiFab,AMREX_SPACEDIM> > m_fluxes;
    m_fluxes.resize(finest_level+1);
    for (int lev=0; lev <= finest_level; ++lev)
    {
        for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) 
        {
             m_fluxes[lev][idim].define(
                    amrex::convert(grids[lev], IntVect::TheDimensionVector(idim)),
                    dmap[lev], 1, 0, MFInfo(), Factory(lev));
        }
    }

    if (m_use_mac_phi_in_godunov)
    {
#ifdef AMREX_USE_EB
        macproj->getFluxes(amrex::GetVecOfArrOfPtrs(m_fluxes), mac_phi, MLMG::Location::FaceCentroid);
#else
        macproj->getFluxes(amrex::GetVecOfArrOfPtrs(m_fluxes), mac_phi, MLMG::Location::FaceCenter);
#endif
    } else {
        for (int lev=0; lev <= finest_level; ++lev)
            for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) 
                 m_fluxes[lev][idim].setVal(0.);
    }

    for (int lev = 0; lev <= finest_level; ++lev) {

        mac_phi[lev]->FillBoundary(geom[lev].periodicity());

#ifdef AMREX_USE_EB
        const EBFArrayBoxFactory* ebfact = &EBFactory(lev);
#endif

        // Predict normal velocity to faces -- note that the {u_mac, v_mac, w_mac}
        //    returned from this call are on face CENTROIDS

        if (m_advection_type == "Godunov") {
#ifdef AMREX_USE_EB
            if (!ebfact->isAllRegular())
                EBGodunov::ExtrapVelToFaces(*vel[lev], *vel_forces[lev], 
                                            AMREX_D_DECL(*u_mac[lev], *v_mac[lev], *w_mac[lev]),
                                            get_velocity_bcrec(), get_velocity_bcrec_device_ptr(),
                                            geom[lev], l_dt);  // Note that PPM not supported for EB
//                                          m_use_mac_phi_in_godunov);
            else
#endif
                Godunov::ExtrapVelToFaces(*vel[lev], *vel_forces[lev], 
                                          AMREX_D_DECL(*u_mac[lev], *v_mac[lev], *w_mac[lev]),
                                          get_velocity_bcrec(), get_velocity_bcrec_device_ptr(),
                                          geom[lev], l_dt, m_godunov_ppm, m_godunov_use_forces_in_trans);
//                                        m_use_mac_phi_in_godunov);
        } else if (m_advection_type == "MOL") {

#ifdef AMREX_USE_EB
            if (!ebfact->isAllRegular())
                EBMOL::ExtrapVelToFaces(*vel[lev], AMREX_D_DECL(*u_mac[lev], *v_mac[lev], *w_mac[lev]), geom[lev],
                                        get_velocity_bcrec(), get_velocity_bcrec_device_ptr());
            else
#endif
                MOL::ExtrapVelToFaces(*vel[lev], AMREX_D_DECL(*u_mac[lev], *v_mac[lev], *w_mac[lev]), geom[lev],
                                       get_velocity_bcrec(), get_velocity_bcrec_device_ptr());

        } else {
            amrex::Abort("Dont know this advection type");
        }
    }
    Vector<Array<MultiFab*,AMREX_SPACEDIM> > mac_vec(finest_level+1);
    for (int lev=0; lev <= finest_level; ++lev)
    {
        AMREX_D_TERM(mac_vec[lev][0] = u_mac[lev];,
                     mac_vec[lev][1] = v_mac[lev];,
                     mac_vec[lev][2] = w_mac[lev];);
    }

    macproj->setUMAC(mac_vec);

    if (m_verbose > 2) amrex::Print() << "MAC Projection:\n";
    //
    // Perform MAC projection
    //
    if (m_use_mac_phi_in_godunov)
    {
        for (int lev=0; lev <= finest_level; ++lev)
            mac_phi[lev]->mult(m_dt/2.,0,1,1);

        macproj->project(mac_phi,m_mac_mg_rtol,m_mac_mg_atol);

        for (int lev=0; lev <= finest_level; ++lev)
            mac_phi[lev]->mult(2./m_dt,0,1,1);
    } else {
        macproj->project(m_mac_mg_rtol,m_mac_mg_atol);
    }
}
