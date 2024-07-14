// ***********************************************************************
// # ExaConstit App
// ## Author: Robert A. Carson
// carson16@llnl.gov
// Steven R. Wopschall
// wopschall1@llnl.gov
// Jamie Bramwell
// bramwell1@llnl.gov
// Date: Aug. 6, 2017
// Updated: Oct. 7, 2019
//
// # Description:
// The purpose of this code app is to determine bulk constitutive properties of metals.
// This is a nonlinear quasi-static, implicit solid mechanics code built on the MFEM library based
// on an updated Lagrangian formulation (velocity based). Currently, only Dirichlet boundary conditions
// (homogeneous and inhomogeneous by dof component) have been implemented. Neumann (traction) boundary
// conditions and a body force are not implemented. A new ExaModel class allows one to implement
// arbitrary constitutive models. The code currently successfully allows for various UMATs to be
// interfaced within the code framework. Development work is currently focused on allowing for the
// mechanical models to run on GPGPUs. The code supports either constant time steps or user supplied
// delta time steps. Boundary conditions are supplied for the velocity field applied on a surface.
// It supports a number of different preconditioned Krylov iterative solvers (PCG, GMRES, MINRES)
// for either symmetric or nonsymmetric positive-definite systems.
//
// ## Remark:
// See the included options.toml to see all of the various different options that are allowable in this
// code and their default values. A TOML parser has been included within this directory, since it has
// an MIT license. The repository for it can be found at: https://github.com/skystrife/cpptoml . Example
// UMATs maybe obtained from https://web.njit.edu/~sac3/Software.html . We have not included them due to
// a question of licensing. The ones that have been run and are known to work are the linear elasticity
// model and the neo-Hookean material. Although, we might be able to provide an example interface so
// users can base their interface/build scripts off of what's known to work.
// Note: the grain.txt, props.txt and state.txt files are expected inputs for CP problems,
// specifically ones that use the Abaqus UMAT interface class under the ExaModel.
//
// # Installing Notes:
// * git clone the LLNL BLT library into cmake directory. It can be obtained at https://github.com/LLNL/blt.git
// * MFEM will need to be built with Conduit (built with HDF5). The easiest way to install Conduit
// is to use spack install instruction provided by Conduit.
// * ExaCMech is required for ExaConstit to be built and can be obtained at https://github.com/LLNL/ExaCMech.git.
// * Create a build directory and cd into there
// * Run ```cmake .. -DENABLE_MPI=ON -DENABLE_FORTRAN=ON -DMFEM_DIR{mfem's installed cmake location}
// -DBLT_SOURCE_DIR=${BLT cloned location} -DECMECH_DIR=${ExaCMech installed cmake location}
// -DRAJA_DIR={RAJA installed location} -DSNLS_DIR={SNLS location in ExaCMech}
// -DMETIS_DIR={Metis used in mfem location} -DHYPRE_DIR={HYPRE install location}
// -DCONDUIT_DIR={Conduit install location} -DHDF5_ROOT:PATH={HDF5 install location}```
// * Run ```make -j 4```
//
// #  Future Implemenations Notes:
// * Visco-plasticity constitutive model
// * GPGPU material models
// * A more in-depth README that better covers the different options available.
// * debug ability to read different mesh formats
// * An up-to-date example options.toml file
// ***********************************************************************
#include "mfem.hpp"
#include "mfem/general/forall.hpp"
#include "mechanics_log.hpp"
#include "system_driver.hpp"
#include "BCData.hpp"
#include "BCManager.hpp"
#include "option_parser.hpp"
#include <string>
#include <sstream>

using namespace std;
using namespace mfem;

// set kinematic functions and boundary condition functions
void ReferenceConfiguration(const Vector &x, Vector &y);
void DirBdrFunc(int attr_id, Vector &y);

// This initializes some grid function
void InitGridFunction(const Vector & /*x*/, Vector &y);

// material input check routine
bool checkMaterialArgs(MechType mt, bool cp, int ngrains, int numProps,
                       int numStateVars);

// material state variable and grain data setter routine
void setStateVarData(Vector* sVars, Vector* orient, ParFiniteElementSpace *fes,
                     int grainOffset, int grainIntoStateVarOffset,
                     int stateVarSize, QuadratureFunction* qf);

// initialize a quadrature function with a single input value, val.
void initQuadFunc(QuadratureFunction *qf, double val);

// initialize a quadrature function that is really a tensor with the identity matrix.
// currently only works for 3x3 tensors.
void initQuadFuncTensorIdentity(QuadratureFunction *qf, ParFiniteElementSpace *fes);

// set the time step on the boundary condition objects
void setBCTimeStep(double dt, int nDBC);

// set the element grain ids from vector data populated from a
// grain map input text file
void setElementGrainIDs(Mesh *mesh, const Vector grainMap, int ncols, int offset);

// used to reset boundary conditions from MFEM convention using
// Make3D() called from the mesh constructor to ExaConstit convention
void setBdrConditions(Mesh *mesh);

// reorder mesh elements in MFEM generated mesh using Make3D() in
// mesh constructor so that the ordering matches the element ordering
// in the input grain map (e.g. from CA calculation)
void reorderMeshElements(Mesh *mesh, const int *nxyz);

// Projects the element attribute to GridFunction nodes
// This also assumes the GridFunction is an L2 FE space
void projectElemAttr2GridFunc(Mesh *mesh, ParGridFunction *elem_attr);

int main(int argc, char *argv[])
{
   CALI_INIT
   CALI_CXX_MARK_FUNCTION;
   CALI_MARK_BEGIN("main_driver_init");
   // Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);
// Used to scope the main program away from the main MPI Init and Finalize calls
{
   // Here we start a timer to time everything
   double start = MPI_Wtime();
   // Here we're going to measure the times of each solve.
   // It'll give us a good idea of strong and weak scaling in
   // comparison to the global value of things.
   // It'll make it easier to point out where some scaling issues might
   // be occurring.
   std::vector<double> times;
   double t1, t2;
   // print the version of the code being run
   if (myid == 0) {
      printf("MFEM Version: %d \n", GetVersion());
   }

   // All of our options are parsed in this file by default
   const char *toml_file = "options.toml";

   // We're going to use the below to allow us to easily swap between different option files
   OptionsParser args(argc, argv);
   args.AddOption(&toml_file, "-opt", "--option", "Option file to use.");
   args.Parse();
   if (!args.Good()) {
      if (myid == 0) {
         args.PrintUsage(cout);
      }
      CALI_MARK_END("main_driver_init");
      MPI_Finalize();
      return 1;
   }

   ExaOptions toml_opt(toml_file);
   toml_opt.parse_options(myid);

   // Set the device info here:
   // Enable hardware devices such as GPUs, and programming models such as
   // CUDA, OCCA, RAJA and OpenMP based on command line options.
   // The current backend priority from highest to lowest is: 'occa-cuda',
   // 'raja-cuda', 'cuda', 'occa-omp', 'raja-omp', 'omp', 'occa-cpu', 'raja-cpu', 'cpu'.

   std::string device_config = "cpu";

   if (toml_opt.rtmodel == RTModel::CPU) {
      device_config = "cpu";
   }
   else if (toml_opt.rtmodel == RTModel::OPENMP) {
      device_config = "raja-omp";
   }
   else if (toml_opt.rtmodel == RTModel::GPU) {
#if defined(RAJA_ENABLE_CUDA) 
      device_config = "raja-cuda";
#elif defined(RAJA_ENABLE_HIP)
      device_config = "raja-hip";
#endif
   }
   Device device;

   if (toml_opt.rtmodel == RTModel::GPU)
   {
      device.SetMemoryTypes(MemoryType::HOST_64, MemoryType::DEVICE);
   }

   device.Configure(device_config.c_str());

   if (myid == 0) {
      printf("\n");
      device.Print();
      printf("\n");
   }
   // Check to see if a custom dt file was used
   // if so read that in and if not set the nsteps that we're going to use
   if (toml_opt.dt_cust) {
      if (myid == 0) {
         printf("Reading in custom dt file. \n");
      }
      ifstream idt(toml_opt.dt_file.c_str());
      if (!idt && myid == 0) {
         cerr << "\nCannot open grain map file: " << toml_opt.grain_map << '\n' << endl;
      }
      // Now we're calculating the final time
      toml_opt.cust_dt.Load(idt, toml_opt.nsteps);
      toml_opt.t_final = 0.0;
      for (int i = 0; i < toml_opt.nsteps; i++) {
         toml_opt.t_final += toml_opt.cust_dt[i];
      }

      idt.close();
   }
   else {
      toml_opt.nsteps = ceil(toml_opt.t_final / toml_opt.dt_min);
      if (myid==0) {
         printf("number of steps %d \n", toml_opt.nsteps);
      }
   }

   times.reserve(toml_opt.nsteps);

   // Check material model argument input parameters for valid combinations
   if (myid == 0) {
      printf("after input before checkMaterialArgs. \n");
   }
   bool err = checkMaterialArgs(toml_opt.mech_type, toml_opt.cp,
                                toml_opt.ngrains, toml_opt.nProps, toml_opt.numStateVars);
   if (!err && myid == 0) {
      cerr << "\nInconsistent material input; check args" << '\n';
   }

   // Open the mesh
   if (myid == 0) {
      printf("before reading the mesh. \n");
   }
   // declare pointer to parallel mesh object
   ParMesh *pmesh = NULL;
   {
      Mesh mesh;
      Vector g_map;
      if ((toml_opt.mesh_type == MeshType::CUBIT) || (toml_opt.mesh_type == MeshType::OTHER)) {
         mesh = Mesh(toml_opt.mesh_file.c_str(), 1, 1, true);
      }
      else {
         if (toml_opt.nxyz[0] <= 0 || toml_opt.mxyz[0] <= 0) {
            cerr << "\nMust input mesh geometry/discretization for hex_mesh_gen" << '\n';
         }

         // use constructor to generate a 3D cuboidal mesh with 8 node hexes
         // The false at the end is to tell the inline mesh generator to use the lexicographic ordering of the mesh
         // The newer space-filling ordering option that was added in the pre-okina tag of MFEM resulted in a noticeable divergence
         // of the material response for a monotonic tension test using symmetric boundary conditions out to 1% strain.
         mesh =
            Mesh::MakeCartesian3D(toml_opt.nxyz[0], toml_opt.nxyz[1], toml_opt.nxyz[2], Element::HEXAHEDRON, 
                                    toml_opt.mxyz[0], toml_opt.mxyz[1], toml_opt.mxyz[2], false);
      }

      // read in the grain map if using a MFEM auto generated cuboidal mesh
      if (toml_opt.mesh_type == MeshType::AUTO) {
         if (myid == 0) {
            printf("using mfem hex mesh generator \n");
         }

         ifstream igmap(toml_opt.grain_map.c_str());
         if (!igmap && myid == 0) {
            cerr << "\nCannot open grain map file: " << toml_opt.grain_map << '\n' << endl;
         }

         int gmapSize = mesh.GetNE();
         g_map.Load(igmap, gmapSize);
         igmap.close();

         //// reorder elements to conform to ordering convention in grain map file
         // No longer needed for the CA stuff. It's now ordered as X->Y->Z
         // reorderMeshElements(mesh, &toml_opt.nxyz[0]);

         // reset boundary conditions from
         setBdrConditions(&mesh);

         // set grain ids as element attributes on the mesh
         // The offset of where the grain index is located is
         // location - 1.
         setElementGrainIDs(&mesh, g_map, 1, 0);
      }

      // We need to check to see if our provided mesh has a different order than
      // the order provided. If we see a difference we either increase our order seen
      // in the options file or we increase the mesh ordering. I'm pretty sure this
      // was causing a problem earlier with our auto-generated mesh and if we wanted
      // to use a higher order FE space.
      // So we can't really do the GetNodalFESpace it appears if we're given
      // an initial mesh. It looks like NodalFESpace is initially set to
      // NULL and only if we swap the mesh nodes does this actually
      // get set...
      // So, we're just going to set the mesh order to at least be 1. Although,
      // I would like to see this change sometime in the future.
      int mesh_order = 1; // mesh->GetNodalFESpace()->GetOrder(0);
      if (mesh_order > toml_opt.order) {
         toml_opt.order = mesh_order;
      }
      if (mesh_order <= toml_opt.order) {
         if (myid == 0) {
            printf("Increasing the order of the mesh to %d\n", toml_opt.order);
         }
         mesh_order = toml_opt.order;
         mesh.SetCurvature(mesh_order);
      }

      // mesh refinement if specified in input
      for (int lev = 0; lev < toml_opt.ser_ref_levels; lev++) {
         mesh.UniformRefinement();
      }

      pmesh = new ParMesh(MPI_COMM_WORLD, mesh);
      for (int lev = 0; lev < toml_opt.par_ref_levels; lev++) {
         pmesh->UniformRefinement();
      }

   } // Mesh related calls
   // Called only once
   {
      BCManager& bcm = BCManager::getInstance();
      bcm.init(toml_opt.updateStep, toml_opt.map_ess_vel, toml_opt.map_ess_vgrad, toml_opt.map_ess_comp,
               toml_opt.map_ess_id);
   }

   CALI_MARK_END("main_driver_init");

   if (myid == 0) {
      printf("after mesh section. \n");
   }

   int dim = pmesh->Dimension();

   // Define the finite element spaces for displacement field
   FiniteElementCollection *fe_coll = NULL;
   fe_coll = new  H1_FECollection(toml_opt.order, dim);
   ParFiniteElementSpace fe_space(pmesh, fe_coll, dim);
   // All of our data is going to be saved off as element average of the field
   // It would be nice if we could have it one day saved off as the raw quadrature
   // fields as well to perform analysis on
   int order_0 = 0;

   // Here we're setting up a discontinuous so that we'll use later to interpolate
   // our quadrature functions from
   L2_FECollection l2_fec(order_0, dim);
   ParFiniteElementSpace l2_fes(pmesh, &l2_fec);
   ParFiniteElementSpace l2_fes_pl(pmesh, &l2_fec, 1);
   ParFiniteElementSpace l2_fes_ori(pmesh, &l2_fec, 4, mfem::Ordering::byVDIM);
   ParFiniteElementSpace l2_fes_cen(pmesh, &l2_fec, dim, mfem::Ordering::byVDIM);
   ParFiniteElementSpace l2_fes_voigt(pmesh, &l2_fec, 6, mfem::Ordering::byVDIM);
   ParFiniteElementSpace l2_fes_tens(pmesh, &l2_fec, 9, mfem::Ordering::byVDIM);
   int gdot_size = 1;
   if(toml_opt.xtal_type == XtalType::FCC || toml_opt.xtal_type == XtalType::BCC) {
      gdot_size = 12;
   } else if (toml_opt.xtal_type == XtalType::HCP) {
      gdot_size = 24;
   }
   ParFiniteElementSpace l2_fes_gdots(pmesh, &l2_fec, gdot_size, mfem::Ordering::byVDIM);

   ParGridFunction vonMises(&l2_fes);
   vonMises = 0.0;
   ParGridFunction volume(&l2_fes);
   ParGridFunction hydroStress(&l2_fes);
   hydroStress = 0.0;
   ParGridFunction stress(&l2_fes_voigt);
   stress = 0.0;
   // Only used for light-up scripts at this point
   ParGridFunction *elem_centroid = nullptr;
   ParGridFunction *elastic_strain = nullptr;
#ifdef MFEM_USE_ADIOS2
   ParGridFunction *elem_attr = nullptr;
   if (toml_opt.adios2) {
      elem_attr = new ParGridFunction(&l2_fes);
      projectElemAttr2GridFunc(pmesh, elem_attr);
   }
#endif

   ParGridFunction dpeff(&l2_fes);
   ParGridFunction pleff(&l2_fes);
   ParGridFunction hardness(&l2_fes);
   ParGridFunction quats(&l2_fes_ori);
   ParGridFunction gdots(&l2_fes);

   if (toml_opt.mech_type == MechType::EXACMECH) {
      dpeff.SetSpace(&l2_fes_pl);
      pleff.SetSpace(&l2_fes_pl);
      // Right now this is only a scalar value but that might change later...
      hardness.SetSpace(&l2_fes_pl);
      quats.SetSpace(&l2_fes_ori);
      gdots.SetSpace(&l2_fes_gdots);
      if (toml_opt.light_up) {
         elem_centroid = new ParGridFunction(&l2_fes_cen);
         elastic_strain = new ParGridFunction(&l2_fes_voigt);
      }
   }

   HYPRE_Int glob_size = fe_space.GlobalTrueVSize();

   pmesh->PrintInfo();

   // Print the mesh statistics
   if (myid == 0) {
      std::cout << "***********************************************************\n";
      std::cout << "dim(u) = " << glob_size << "\n";
      std::cout << "***********************************************************\n";
   }

   // determine the type of grain input for crystal plasticity problems
   int ori_offset = 0; // note: numMatVars >= 1, no null state vars by construction
   if (toml_opt.cp) {
      if (toml_opt.ori_type == OriType::EULER) {
         ori_offset = 3;
      }
      else if (toml_opt.ori_type == OriType::QUAT) {
         ori_offset = 4;
      }
      else if (toml_opt.ori_type == OriType::CUSTOM) {
         if (toml_opt.grain_custom_stride == 0) {
            cerr << "\nMust specify a grain stride for grain_custom input" << '\n';
         }
         ori_offset = toml_opt.grain_custom_stride;
      }
   }

   // set the offset for the matVars quadrature function. This is the number of
   // state variables (stored at each integration point) and then the grain offset,
   // which is the number of variables defining the grain data stored at each
   // integration point. In general, these may come in as different data sets,
   // even though they will be stored in a single material state variable
   // quadrature function.
   int matVarsOffset = toml_opt.numStateVars + ori_offset;

   // Define a quadrature space and material history variable QuadratureFunction.
   int intOrder = 2 * toml_opt.order + 1;
   QuadratureSpace qspace(pmesh, intOrder); // 3rd order polynomial for 2x2x2 quadrature
                                            // for first order finite elements.
   QuadratureFunction matVars0(&qspace, matVarsOffset);
   initQuadFunc(&matVars0, 0.0);

   // Used for post processing steps
   QuadratureSpace qspace0(pmesh, 1);
   QuadratureFunction elemMatVars(&qspace0, matVarsOffset);
   elemMatVars = 0.0;

   // read in material properties and state variables files for use with ALL models
   // store input data on Vector object. The material properties vector will be
   // passed into the Nonlinear mech operator constructor to initialize the material
   // properties vector on the model and the state variables vector will be used with
   // the grain data vector (if crystal plasticity) to populate the material state
   // vector quadrature function. It is assumed that the state variables input file
   // are initial values for all state variables applied to all quadrature points.
   // There is not a separate initialization file for each quadrature point
   Vector matProps;
   Vector stateVars;
   if (myid == 0) {
      printf("before reading in matProps and stateVars. \n");
   }
   { // read in props, material state vars and grains if crystal plasticity
      ifstream iprops(toml_opt.props_file.c_str());
      if (!iprops && myid == 0) {
         cerr << "\nCannot open material properties file: " << toml_opt.props_file << '\n' << endl;
      }

      // load material properties
      matProps.Load(iprops, toml_opt.nProps);
      iprops.close();

      if (myid == 0) {
         printf("after loading matProps. \n");
      }

      // read in state variables file
      ifstream istateVars(toml_opt.state_file.c_str());
      if (!istateVars && myid == 0) {
         cerr << "\nCannot open state variables file: " << toml_opt.state_file << '\n' << endl;
      }

      // load state variables
      stateVars.Load(istateVars, toml_opt.numStateVars);
      istateVars.close();
      if (myid == 0) {
         printf("after loading stateVars. \n");
      }

      // if using a crystal plasticity model then get grain orientation data
      // declare a vector to hold the grain orientation input data. This data is per grain
      // with a stride set previously as grain_offset
      Vector g_orient;
      if (myid == 0) {
         printf("before loading g_orient. \n");
      }
      if (toml_opt.cp) {
         // set the grain orientation vector from the input grain file
         ifstream igrain(toml_opt.ori_file.c_str());
         if (!igrain && myid == 0) {
            cerr << "\nCannot open orientation file: " << toml_opt.ori_file << '\n' << endl;
         }
         // load separate grain file
         int gsize = ori_offset * toml_opt.ngrains;
         g_orient.Load(igrain, gsize);
         igrain.close();
         if (myid == 0) {
            printf("after loading g_orient. \n");
         }
      } // end if (cp)

      // set the state var data on the quadrature function
      if (myid == 0) {
         printf("before setStateVarData. \n");
      }
      setStateVarData(&stateVars, &g_orient, &fe_space, ori_offset,
                      toml_opt.grain_statevar_offset, toml_opt.numStateVars, &matVars0);
      if (myid == 0) {
         printf("after setStateVarData. \n");
      }
   } // end read of mat props, state vars and grains

   // Declare quadrature functions to store a vector representation of the
   // Cauchy stress, in Voigt notation (s_11, s_22, s_33, s_23, s_13, s_12), for
   // the beginning of the step and the end of the step.
   int stressOffset = 6;
   QuadratureFunction sigma0(&qspace, stressOffset);
   QuadratureFunction sigma1(&qspace, stressOffset);
   QuadratureFunction q_vonMises(&qspace, 1);
   initQuadFunc(&sigma0, 0.0);
   initQuadFunc(&sigma1, 0.0);
   initQuadFunc(&q_vonMises, 0.0);

   // The tangent stiffness of the Cauchy stress will
   // actually be the real material tangent stiffness (4th order tensor) and have
   // 36 components due to symmetry.
   int matGradOffset = 36;
   QuadratureFunction matGrd(&qspace, matGradOffset);
   initQuadFunc(&matGrd, 0.0);

   // define the end of step (or incrementally updated) material history
   // variables
   int vdim = matVars0.GetVDim();
   QuadratureFunction matVars1(&qspace, vdim);
   initQuadFunc(&matVars1, 0.0);

   // declare a quadrature function to store the beginning step kinematic variables
   // for any incremental kinematics. Right now this is used to store the beginning
   // step deformation gradient on the model.
   int kinDim = 9;
   QuadratureFunction kinVars0(&qspace, kinDim);
   initQuadFuncTensorIdentity(&kinVars0, &fe_space);

   // Define a grid function for the global reference configuration, the beginning
   // step configuration, the global deformation, the current configuration/solution
   // guess, and the incremental nodal displacements
   ParGridFunction x_ref(&fe_space);
   ParGridFunction x_beg(&fe_space);
   ParGridFunction x_cur(&fe_space);
   // x_diff would be our displacement
   ParGridFunction x_diff(&fe_space);
   ParGridFunction v_cur(&fe_space);

   // define a vector function coefficient for the initial deformation
   // (based on a velocity projection) and reference configuration.
   // Additionally define a vector function coefficient for computing
   // the grid velocity prior to a velocity projection
   VectorFunctionCoefficient refconfig(dim, ReferenceConfiguration);

   // Initialize the reference and beginning step configuration grid functions
   // with the refconfig vector function coefficient.
   x_beg.ProjectCoefficient(refconfig);
   x_ref.ProjectCoefficient(refconfig);
   x_cur.ProjectCoefficient(refconfig);

   // Define grid function for the velocity solution grid function
   // WITH Dirichlet BCs

   // Define a VectorFunctionCoefficient to initialize a grid function
   VectorFunctionCoefficient init_grid_func(dim, InitGridFunction);

   // initialize boundary condition, velocity, and
   // incremental nodal displacment grid functions by projection the
   // VectorFunctionCoefficient function onto them
   x_diff.ProjectCoefficient(init_grid_func);
   v_cur.ProjectCoefficient(init_grid_func);

   // Construct the nonlinear mechanics operator. Note that q_grain0 is
   // being passed as the matVars0 quadarture function. This is the only
   // history variable considered at this moment. Consider generalizing
   // this where the grain info is a possible subset only of some
   // material history variable quadrature function. Also handle the
   // case where there is no grain data.
   if (myid == 0) {
      printf("before SystemDriver constructor. \n");
   }

   // Now to make sure all of our state variables and other such type of variables are on the device.
   // If we don't do the below than whenever var = #.# for example will occur back on the host and then
   // brought back to the device.
   matVars0.UseDevice(true);
   matVars1.UseDevice(true);
   sigma0.UseDevice(true);
   sigma1.UseDevice(true);
   matGrd.UseDevice(true);
   kinVars0.UseDevice(true);
   q_vonMises.UseDevice(true);
   matProps.UseDevice(true);

   {
      // fix me: should the mesh nodes be on the device?
      GridFunction *nodes = &x_cur; // set a nodes grid function to global current configuration
      int owns_nodes = 0;
      pmesh->SwapNodes(nodes, owns_nodes); // pmesh has current configuration nodes
      nodes = NULL;
   }

   SystemDriver oper(fe_space,
                     toml_opt, matVars0,
                     matVars1, sigma0, sigma1, matGrd,
                     kinVars0, q_vonMises, &elemMatVars, x_ref, x_beg, x_cur,
                     matProps, matVarsOffset);

   if (toml_opt.visit || toml_opt.conduit || toml_opt.paraview || toml_opt.adios2) {
      oper.ProjectVolume(volume);
   }
   if (myid == 0) {
      printf("after SystemDriver constructor. \n");
   }

   // get the essential true dof list. This may not be used.
   const Array<int> ess_tdof_list = oper.GetEssTDofList();

   // declare incremental nodal displacement solution vector
   Vector v_sol(fe_space.TrueVSize()); v_sol.UseDevice(true);
   Vector v_prev(fe_space.TrueVSize()); v_prev.UseDevice(true);// this sizing is correct
   v_sol = 0.0;

   // Save data for VisIt visualization.
   // The below is used to take advantage of mfem's custom Visit plugin
   // It could also allow for restart files later on.
   // If we have large simulations although the current method of printing everything
   // as text will cause issues. The data should really be saved in some binary format.
   // If you don't then you'll often find that the printed data lags behind where
   // the simulation is currently at. This really becomes noticiable if you have
   // a lot of data that you want to output for the user. It might be nice if this
   // was either a netcdf or hdf5 type format instead.
   CALI_MARK_BEGIN("main_vis_init");
   VisItDataCollection visit_dc(toml_opt.basename, pmesh);
   ParaViewDataCollection paraview_dc(toml_opt.basename, pmesh);
#ifdef MFEM_USE_CONDUIT
   ConduitDataCollection conduit_dc(toml_opt.basename, pmesh);
#endif
#ifdef MFEM_USE_ADIOS2
   const std::string basename = toml_opt.basename + ".bp";
   ADIOS2DataCollection *adios2_dc = new ADIOS2DataCollection(MPI_COMM_WORLD, basename, pmesh);
#endif
   if (toml_opt.paraview) {
      paraview_dc.SetLevelsOfDetail(toml_opt.order);
      paraview_dc.SetDataFormat(VTKFormat::BINARY);
      paraview_dc.SetHighOrderOutput(false);

      paraview_dc.RegisterField("ElementVolume", &volume);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         if(toml_opt.light_up) {
            oper.ProjectCentroid(*elem_centroid);
            oper.ProjectElasticStrains(*elastic_strain);
            oper.ProjectOrientation(quats);
            paraview_dc.RegisterField("ElemCentroid", elem_centroid);
            paraview_dc.RegisterField("XtalElasticStrain", elastic_strain);
            paraview_dc.RegisterField("LatticeOrientation", &quats);
         }
      }

      paraview_dc.SetCycle(0);
      paraview_dc.SetTime(0.0);
      paraview_dc.Save();

      paraview_dc.RegisterField("Displacement", &x_diff);
      paraview_dc.RegisterField("Stress", &stress);
      paraview_dc.RegisterField("Velocity", &v_cur);
      paraview_dc.RegisterField("VonMisesStress", &vonMises);
      paraview_dc.RegisterField("HydrostaticStress", &hydroStress);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         // We also want to project the values out originally
         // so our initial values are correct
         oper.ProjectDpEff(dpeff);
         oper.ProjectEffPlasticStrain(pleff);
         oper.ProjectOrientation(quats);
         oper.ProjectShearRate(gdots);
         oper.ProjectH(hardness);

         paraview_dc.RegisterField("DpEff", &dpeff);
         paraview_dc.RegisterField("EffPlasticStrain", &pleff);
         if(!toml_opt.light_up) {
            paraview_dc.RegisterField("LatticeOrientation", &quats);
         }
         paraview_dc.RegisterField("ShearRate", &gdots);
         paraview_dc.RegisterField("Hardness", &hardness);
      }
   }

   if (toml_opt.visit) {
      visit_dc.SetPrecision(12);

      visit_dc.RegisterField("ElementVolume", &volume);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         if(toml_opt.light_up) {
            oper.ProjectCentroid(*elem_centroid);
            oper.ProjectElasticStrains(*elastic_strain);
            oper.ProjectOrientation(quats);
            visit_dc.RegisterField("ElemCentroid", elem_centroid);
            visit_dc.RegisterField("XtalElasticStrain", elastic_strain);
            visit_dc.RegisterField("LatticeOrientation", &quats);
         }
      }

      visit_dc.SetCycle(0);
      visit_dc.SetTime(0.0);
      visit_dc.Save();

      visit_dc.RegisterField("Displacement", &x_diff);
      visit_dc.RegisterField("Stress", &stress);
      visit_dc.RegisterField("Velocity", &v_cur);
      visit_dc.RegisterField("VonMisesStress", &vonMises);
      visit_dc.RegisterField("HydrostaticStress", &hydroStress);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         // We also want to project the values out originally
         // so our initial values are correct

         oper.ProjectDpEff(dpeff);
         oper.ProjectEffPlasticStrain(pleff);
         oper.ProjectOrientation(quats);
         oper.ProjectShearRate(gdots);
         oper.ProjectH(hardness);

         visit_dc.RegisterField("DpEff", &dpeff);
         visit_dc.RegisterField("EffPlasticStrain", &pleff);
         if(!toml_opt.light_up) {
            visit_dc.RegisterField("LatticeOrientation", &quats);
         }
         visit_dc.RegisterField("ShearRate", &gdots);
         visit_dc.RegisterField("Hardness", &hardness);
      }
   }

#ifdef MFEM_USE_CONDUIT
   if (toml_opt.conduit) {
      // conduit_dc.SetProtocol("json");
      conduit_dc.RegisterField("ElementVolume", &volume);

      conduit_dc.SetCycle(0);
      conduit_dc.SetTime(0.0);
      conduit_dc.Save();

      conduit_dc.RegisterField("Displacement", &x_diff);
      conduit_dc.RegisterField("Stress", &stress);
      conduit_dc.RegisterField("Velocity", &v_cur);
      conduit_dc.RegisterField("VonMisesStress", &vonMises);
      conduit_dc.RegisterField("HydrostaticStress", &hydroStress);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         // We also want to project the values out originally
         // so our initial values are correct
         oper.ProjectDpEff(dpeff);
         oper.ProjectEffPlasticStrain(pleff);
         oper.ProjectOrientation(quats);
         oper.ProjectShearRate(gdots);
         oper.ProjectH(hardness);

         conduit_dc.RegisterField("DpEff", &dpeff);
         conduit_dc.RegisterField("EffPlasticStrain", &pleff);
         conduit_dc.RegisterField("LatticeOrientation", &quats);
         conduit_dc.RegisterField("ShearRate", &gdots);
         conduit_dc.RegisterField("Hardness", &hardness);
      }
   }
#endif
#ifdef MFEM_USE_ADIOS2
   if (toml_opt.adios2) {
      adios2_dc->SetParameter("SubStreams", std::to_string(num_procs / 2) );

      adios2_dc->RegisterField("ElementAttribute", elem_attr);
      adios2_dc->RegisterField("ElementVolume", &volume);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         if(toml_opt.light_up) {
            oper.ProjectCentroid(*elem_centroid);
            oper.ProjectElasticStrains(*elastic_strain);
            oper.ProjectOrientation(quats);
            adios2_dc->RegisterField("ElemCentroid", elem_centroid);
            adios2_dc->RegisterField("XtalElasticStrain", elastic_strain);
            adios2_dc->RegisterField("LatticeOrientation", &quats);
         }
      }

      adios2_dc->SetCycle(0);
      adios2_dc->SetTime(0.0);
      adios2_dc->Save();

      adios2_dc->DeregisterField("ElementAttribute");
      adios2_dc->RegisterField("Displacement", &x_diff);
      adios2_dc->RegisterField("Stress", &stress);
      adios2_dc->RegisterField("Velocity", &v_cur);
      adios2_dc->RegisterField("VonMisesStress", &vonMises);
      adios2_dc->RegisterField("HydrostaticStress", &hydroStress);

      if (toml_opt.mech_type == MechType::EXACMECH) {
         // We also want to project the values out originally
         // so our initial values are correct
         oper.ProjectDpEff(dpeff);
         oper.ProjectEffPlasticStrain(pleff);
         oper.ProjectOrientation(quats);
         oper.ProjectShearRate(gdots);
         oper.ProjectH(hardness);

         adios2_dc->RegisterField("DpEff", &dpeff);
         adios2_dc->RegisterField("EffPlasticStrain", &pleff);
         // We should already have this registered if using the light-up script
         if(!toml_opt.light_up) {
            adios2_dc->RegisterField("LatticeOrientation", &quats);
         }
         adios2_dc->RegisterField("ShearRate", &gdots);
         adios2_dc->RegisterField("Hardness", &hardness);
      }
   }
#endif
   if (myid == 0) {
      printf("after visualization if-block \n");
   }
   CALI_MARK_END("main_vis_init");
   // initialize/set the time
   double t = 0.0;
   oper.SetTime(t);

   bool last_step = false;

   double dt_real;

   for (int ti = 1; ti <= toml_opt.nsteps; ti++) {
      if (myid == 0) {
         printf("inside timestep loop %d \n", ti);
      }
      // Get out our current delta time step
      if (toml_opt.dt_cust) {
         dt_real = toml_opt.cust_dt[ti - 1];
      }
      else if (toml_opt.dt_auto) {
         const double dt_system = oper.GetDt();
         dt_real = min(dt_system, toml_opt.t_final - t);
      }
      else {
         dt_real = min(toml_opt.dt, toml_opt.t_final - t);
      }

      // compute current time
      t = t + dt_real;
      last_step = (std::abs(t - toml_opt.t_final) <= std::abs(1e-3 * dt_real));

      // set time on the simulation variables and the model through the
      // nonlinear mechanics operator class
      oper.SetTime(t);
      oper.SetDt(dt_real);
      oper.solVars.SetLastStep(last_step);

      // If our boundary condition changes for a step, we need to have an initial
      // corrector step that ensures the solver has an easier time solving the PDE.
      t1 = MPI_Wtime();
      if (BCManager::getInstance().getUpdateStep(ti)) {
         if (myid == 0) {
            std::cout << "Changing boundary conditions this step: " << ti << std::endl;
         }
         v_prev = v_sol;
         // Update the BC data
         oper.UpdateEssBdr();
         oper.UpdateVelocity(v_cur, v_sol);
         oper.SolveInit(v_prev, v_sol);
         // oper.SolveInit(v_sol);
         // distribute the solution vector to v_cur
         v_cur.Distribute(v_sol);
      }
      oper.UpdateVelocity(v_cur, v_sol);
      // This will always occur
      oper.Solve(v_sol);

      // Our expected dt could have changed
      if (toml_opt.dt_auto) {
         t = oper.solVars.GetTime();
         dt_real = oper.solVars.GetDTime();
         // Check to see if this has changed or not
         last_step = (std::abs(t - toml_opt.t_final) <= std::abs(1e-3 * dt_real));
      }

      t2 = MPI_Wtime();
      times[ti - 1] = t2 - t1;

      // distribute the solution vector to v_cur
      v_cur.Distribute(v_sol);

      // find the displacement vector as u = x_cur - x_reference
      subtract(x_cur, x_ref, x_diff);
      // update the beginning step stress and material state variables
      // prior to the next time step for all Exa material models
      // This also updates the deformation gradient with the beginning step
      // deformation gradient stored on an Exa model

      oper.UpdateModel();

      // Update our beginning time step coords with our end time step coords
      x_beg = x_cur;

      if (last_step || (ti % toml_opt.vis_steps) == 0) {
         if (myid == 0) {
            cout << "step " << ti << ", t = " << t << endl;
         }
         CALI_MARK_BEGIN("main_vis_update");
         if (toml_opt.visit || toml_opt.conduit || toml_opt.paraview || toml_opt.adios2) {
            // mesh and stress output. Consider moving this to a separate routine
            // We might not want to update the vonMises stuff
            oper.ProjectModelStress(stress);
            oper.ProjectVolume(volume);
            oper.ProjectVonMisesStress(vonMises, stress);
            oper.ProjectHydroStress(hydroStress, stress);

            if (toml_opt.mech_type == MechType::EXACMECH) {
               if(toml_opt.light_up) {
                  oper.ProjectCentroid(*elem_centroid);
                  oper.ProjectElasticStrains(*elastic_strain);
               }
               oper.ProjectDpEff(dpeff);
               oper.ProjectEffPlasticStrain(pleff);
               oper.ProjectOrientation(quats);
               oper.ProjectShearRate(gdots);
               oper.ProjectH(hardness);
            }
         }

         if (toml_opt.visit) {
            visit_dc.SetCycle(ti);
            visit_dc.SetTime(t);
            // Our visit data is now saved off
            visit_dc.Save();
         }
         if (toml_opt.paraview) {
            paraview_dc.SetCycle(ti);
            paraview_dc.SetTime(t);
            // Our paraview data is now saved off
            paraview_dc.Save();
         }
#ifdef MFEM_USE_CONDUIT
         if (toml_opt.conduit) {
            conduit_dc.SetCycle(ti);
            conduit_dc.SetTime(t);
            // Our conduit data is now saved off
            conduit_dc.Save();
         }
#endif
#ifdef MFEM_USE_ADIOS2
         if (toml_opt.adios2) {
            adios2_dc->SetCycle(ti);
            adios2_dc->SetTime(t);
            // Our adios2 data is now saved off
            adios2_dc->Save();
         }
#endif
         CALI_MARK_END("main_vis_update");
      } // end output scope
      if (last_step) {
         break;
      }
   } // end loop over time steps

   // Free the used memory.
   delete pmesh;
   // Now find out how long everything took to run roughly
   double end = MPI_Wtime();

   double sim_time = end - start;
   double avg_sim_time;

   MPI_Allreduce(&sim_time, &avg_sim_time, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
   int world_size;
   MPI_Comm_size(MPI_COMM_WORLD, &world_size);

   {
      std::ostringstream oss;

      oss << "./time/time_solve." << myid << ".txt";
      std::string file_name = oss.str();
      std::ofstream file;
      file.open(file_name, std::ios::out | std::ios::app);

      for (int i = 0; i < toml_opt.nsteps; i++) {
         std::ostringstream strs;
         strs << setprecision(8) << times[i] << "\n";
         std::string str = strs.str();
         file << str;
      }

      file.close();
   }


   if (myid == 0) {
      printf("The process took %lf seconds to run\n", (avg_sim_time / world_size));
   }

   if(toml_opt.light_up) {
      delete elem_centroid;
      delete elastic_strain;
   }

#ifdef MFEM_USE_ADIOS2
   if (toml_opt.adios2) {
      delete elem_attr;
   }
   delete adios2_dc;
#endif

} // Used to ensure any mpi functions are scopped to only this section
   MPI_Barrier(MPI_COMM_WORLD);
   MPI_Finalize();

   return 0;
}

void ReferenceConfiguration(const Vector &x, Vector &y)
{
   // set the reference, stress free, configuration
   y = x;
}

void InitGridFunction(const Vector & /*x*/, Vector &y)
{
   y = 0.;
}

bool checkMaterialArgs(MechType mt, bool cp, int ngrains, int numProps,
                       int numStateVars)
{
   bool err = true;

   if (cp && (ngrains < 1)) {
      cerr << "\nSpecify number of grains for use with cp input arg." << '\n';
      err = false;
   }

   if (mt !=  MechType::NOTYPE && (numProps < 1)) {
      cerr << "\nMust specify material properties for mechanical model or cp calculation." << '\n';
      err = false;
   }

   // always input a state variables file with initial values for all models
   if (numStateVars < 1) {
      cerr << "\nMust specifiy state variables." << '\n';
   }

   return err;
}

void setStateVarData(Vector* sVars, Vector* orient, ParFiniteElementSpace *fes,
                     int grainSize, int grainIntoStateVarOffset,
                     int stateVarSize, QuadratureFunction* qf)
{
   // put element grain orientation data on the quadrature points.
   const IntegrationRule *ir;
   double* qf_data = qf->HostReadWrite();
   int qf_offset = qf->GetVDim(); // offset = grainSize + stateVarSize
   QuadratureSpaceBase* qspace = qf->GetSpace();

   int myid;
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // check to make sure the sum of the input sizes matches the offset of
   // the input quadrature function
   if (qf_offset != (grainSize + stateVarSize)) {
      if (myid == 0) {
         cerr << "\nsetStateVarData: Input state variable and grain sizes do not "
            "match quadrature function initialization." << '\n';
      }
   }

   // get the data for the material state variables and grain orientations for
   // nonzero grainSize(s), which implies a crystal plasticity calculation
   double* grain_data = NULL;
   if (grainSize > 0) {
      grain_data = orient->HostReadWrite();
   }

   double* sVars_data = sVars->HostReadWrite();
   int elem_atr;

   int offset1;
   int offset2;
   if (grainIntoStateVarOffset < 0) { // put grain data at end
      // print warning to screen since this case could arise from a user
      // simply not setting this parameter
      if (myid == 0) {
         std::cout << "warning::setStateVarData grain data placed at end of"
                   << " state variable array. Check grain_statevar_offset input arg." << "\n";
      }

      offset1 = stateVarSize - 1;
      offset2 = qf_offset;
   }
   else if (grainIntoStateVarOffset == 0) { // put grain data at beginning
      offset1 = -1;
      offset2 = grainSize;
   }
   else { // put grain data somewhere in the middle
      offset1 = grainIntoStateVarOffset - 1;
      offset2 = grainIntoStateVarOffset + grainSize;
   }

   // loop over elements
   for (int i = 0; i < fes->GetNE(); ++i) {
      ir = &(qspace->GetIntRule(i));

      // full history variable offset including grain data
      int elem_offset = qf_offset * ir->GetNPoints();

      // get the element attribute. Note this assumes that there is an element attribute
      // for all elements in the mesh corresponding to the grain id to which the element
      // belongs.
      elem_atr = fes->GetAttribute(i) - 1;
      // loop over quadrature points
      for (int j = 0; j < ir->GetNPoints(); ++j) {
         // loop over quadrature point material state variable data
         double varData;
         int igrain = 0;
         int istateVar = 0;
         for (int k = 0; k < qf_offset; ++k) {
            // index into either the grain data or the material state variable
            // data depending on the setting of offset1 and offset2. This handles
            // tacking on the grain data at the beginning of the total material
            // state variable quadarture function, the end, or somewhere in the
            // middle, which is dictated by grainIntoStateVarOffset, which is
            // ultimately a program input. If grainSize == 0 for non-crystal
            // plasticity problems, we never get into the if-block that gets
            // data from the grain_data. In fact, grain_data should be a null
            // pointer
            if (k > offset1 && k < offset2) {
               varData = grain_data[grainSize * (elem_atr) + igrain];
               ++igrain;
            }
            else {
               varData = sVars_data[istateVar];
               ++istateVar;
            }

            qf_data[(elem_offset * i) + qf_offset * j + k] = varData;
         } // end loop over material state variables
      } // end loop over quadrature points
   } // end loop over elements

   // Set the pointers to null after using them to hopefully stop any weirdness from happening
}

void initQuadFunc(QuadratureFunction *qf, double val)
{
   double* qf_data = qf->ReadWrite();
   const int npts = qf->Size();

   // The below should be exactly the same as what
   // the other for loop is trying to accomplish
   MFEM_FORALL(i, npts, {
      qf_data[i] = val;
   });
}

void initQuadFuncTensorIdentity(QuadratureFunction *qf, ParFiniteElementSpace *fes)
{
   double* qf_data = qf->ReadWrite();
   const int qf_offset = qf->GetVDim(); // offset at each integration point
   QuadratureSpaceBase* qspace = qf->GetSpace();
   const IntegrationRule *ir = &(qspace->GetIntRule(0));
   const int int_pts = ir->GetNPoints();
   const int nelems = fes->GetNE();

   // loop over elements
   MFEM_FORALL(i, nelems, {
      const int elem_offset = qf_offset * int_pts;
      // Hard coded this for now for a 3x3 matrix
      // Fix later if we update
      for (int j = 0; j < int_pts; ++j) {
         qf_data[i * elem_offset + j * qf_offset] = 1.0;
         qf_data[i * elem_offset + j * qf_offset + 1] = 0.0;
         qf_data[i * elem_offset + j * qf_offset + 2] = 0.0;
         qf_data[i * elem_offset + j * qf_offset + 3] = 0.0;
         qf_data[i * elem_offset + j * qf_offset + 4] = 1.0;
         qf_data[i * elem_offset + j * qf_offset + 5] = 0.0;
         qf_data[i * elem_offset + j * qf_offset + 6] = 0.0;
         qf_data[i * elem_offset + j * qf_offset + 7] = 0.0;
         qf_data[i * elem_offset + j * qf_offset + 8] = 1.0;
      }
   });
}

void setBdrConditions(Mesh *mesh)
{
   // modify MFEM auto cuboidal hex mesh generation boundary
   // attributes to correspond to correct ExaConstit boundary conditions.
   // Look at ../../mesh/mesh.cpp Make3D() to see how boundary attributes
   // are set and modify according to ExaConstit convention

   // loop over boundary elements
   for (int i = 0; i<mesh->GetNBE(); ++i) {
      int bdrAttr = mesh->GetBdrAttribute(i);

      switch (bdrAttr) {
         // note, srw wrote SetBdrAttribute() in ../../mesh/mesh.hpp
         case 1:
            mesh->SetBdrAttribute(i, 1); // bottom
            break;
         case 2:
            mesh->SetBdrAttribute(i, 3); // front
            break;
         case 3:
            mesh->SetBdrAttribute(i, 5); // right
            break;
         case 4:
            mesh->SetBdrAttribute(i, 6); // back
            break;
         case 5:
            mesh->SetBdrAttribute(i, 2); // left
            break;
         case 6:
            mesh->SetBdrAttribute(i, 4); // top
            break;
      }
   }

   return;
}

void reorderMeshElements(Mesh *mesh, const int *nxyz)
{
   // reorder mesh elements depending on how the
   // computational cells are ordered in the grain map file.

   // Right now, the element ordering in the grain map file
   // starts at (0,0,0) and increments in z, y, then x coordinate
   // directions.

   // MFEM Make3D(.) mesh gen increments in x, y, then z.

   Array<int> order(nxyz[0] * nxyz[1] * nxyz[2]);
   int id = 0;
   int k = 0;
   for (int z = 0; z < nxyz[2]; ++z) {
      for (int y = 0; y < nxyz[1]; ++y) {
         for (int x = 0; x < nxyz[0]; ++x) {
            id = (nxyz[2] * nxyz[1]) * x + nxyz[2] * y + z;
            order[k] = id;
            ++k;
         }
      }
   }

   mesh->ReorderElements(order, true);

   return;
}

void setElementGrainIDs(Mesh *mesh, const Vector grainMap, int ncols, int offset)
{
   // after a call to reorderMeshElements, the elements in the serial
   // MFEM mesh should be ordered the same as the input grainMap
   // vector. Set the element attribute to the grain id. This vector
   // has stride of 4 with the id in the 3rd position indexing from 0

   const double* data = grainMap.HostRead();

   // loop over elements
   for (int i = 0; i<mesh->GetNE(); ++i) {
      mesh->SetAttribute(i, data[ncols * i + offset]);
   }

   return;
}

// Projects the element attribute to GridFunction nodes
// This also assumes this the GridFunction is an L2 FE space
void projectElemAttr2GridFunc(Mesh *mesh, ParGridFunction *elem_attr) {
   // loop over elementsQ
   elem_attr->HostRead();
   ParFiniteElementSpace *pfes = elem_attr->ParFESpace();
   Array<int> vdofs;
   for (int i = 0; i < mesh->GetNE(); ++i) {
      pfes->GetElementVDofs(i, vdofs);
      const double ea = static_cast<double>(mesh->GetAttribute(i));
      elem_attr->SetSubVector(vdofs, ea);
   }
}
