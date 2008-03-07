/**

@mainpage

\section Introduction

This program created a corner detector by optimizing a decision tree using simulated annealing.
The tree is optimized to maximize the repeatability of the corner detector.

\section Instructions

To make on any unix-like environment, do:

<code>./configure @@ make</code>

There is no install option. To set the parameters, examine <code>learn_detector.cfg</code>.
In order to run this program, you will need a repeatability dataset, such as the one from
http://mi.eng.cam.ac.uk/~er258/work/datasets.html

The running program will generate a very extensive logfile on the standard
output, which will include the learned detector and the results of its
repeatability evaluation. Running <code>get_block_detector</code> on the output
will generate source code for the detector. Note that this source code will not
yet have been optimized for speed, only repeatability.

The complete sequence of operations is as follows
<ol>
	<li> Make the executable:

		<code> ./configure && make </code>

	<li> Set up <code>learn_detector.cfg</code>. The default parameters are good,
		 except you will need to set up <code>datadir</code> to point to the
		 repeatability dataset.

	<li> Run the corner detector learning program

		<code>./learn_detector > logfile</code>

		If you run it more than once, you will probably want to alter the random
		seed in the configuration file.

	<li> Extract a detector from the logfile

	     <code>./get_block_detector logfile &gt; learned-faster-detector.h</code>

	<li> make <code>extract-fast2.cxx</code>
		
		Note that if you have changed the list of available offsets in
		<code>learn_detector.cfg</code> then you will need to change
		<code>offset_list</code> in <code>extract-fast2.cxx</code>. The list of
		offsets will be in the logfile.

	<li> Use <code>extract-fast2.cxx</code> to extract features from a set of
	     training images. Any reasonable images can be used, including the 
		 training images used earlier.  Do:

		 <code>./extract-fast2<code><i>imagefile  imagefile2 ...<i><code>&gt; features</code>
</ol>

*/

/**
@defgroup gRepeatability Measuring the repeatability of a detector

Functions to load a repeatability dataset, and compute the repeatability of
a list of detected points.


\section data The dataset

The dataset consists of a number of registered images. The images are stored in
<code>frames/frame_X.pgm</code> where X is an integer counting from zero.  The
frames must all be the same size. The warps are stored in
<code>waprs/warp_Y_Z.warp</code>. The file <code>warp_Y_Z.warp</code> contains
one line for every pixel in image Y (pixels arranged in raster-scan order). The
line is the position that the pixel warps to in image Z. If location of -1, -1
indicates that this pixel does not appear in image Z.

*/

/**
@defgroup  gTree Tree representation.

*/

/**
@defgroup  gFastTree Compiled tree representations

*/

/**
@defgroup  gUtility Utility functions.

*/

/**
@defgroup  gOptimize Optimization routines

*/
#include <iostream>
#include <fstream>
#include <climits>
#include <float.h>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <vector>
#include <utility>
#include <algorithm>

#include <cvd/image_io.h>
#include <cvd/random.h>
#include <cvd/vector_image_ref.h>

#include <tag/tuple.h>
#include <tag/stdpp.h>
#include <tag/fn.h>
#include <tag/printf.h>

#include <TooN/TooN.h>

#include "gvars_vector.h"
#include "faster_tree.h"
#include "faster_bytecode.h"
#include "offsets.h"

using namespace std;
using namespace CVD;
using namespace tag;
using namespace GVars3;
using namespace TooN;
////////////////////////////////////////////////////////////////////////////////
//
// Utility functions
//

///Square a number
///@param d Number to square
///@return  $d^2$
///@ingroup gUtility
double sq(double d)
{
	return d*d;
}


///Populate a std::vector with the numbers 0,1,...,num
///@param num Size if the range
///@return the populated vector.
///@ingroup Utility
vector<int> range(int num)
{
	vector<int> r;

	for(int i=0; i < num; i++)
		r.push_back(i);
	return r;
}



////////////////////////////////////////////////////////////////////////////////
//
//  Functions related to repeatability 
//


///Generate a disc of ImageRefs.
///@param radius Radius of the disc
///@return the disc of ImageRefs
///@ingroup gRepeatability
vector<ImageRef> generate_disc(int radius)
{
	vector<ImageRef> ret;
	ImageRef p;

	for(p.y = -radius; p.y <= radius; p.y++)
		for(p.x = -radius; p.x <= radius; p.x++)
			if((int)p.mag_squared() <= radius)
				ret.push_back(p);
	return ret;
}


///Paint shapes (a vector<ImageRef>) safely in to an image
///This is used to paint discs at corner locations in order to 
///perform rapid proximity checking.
///
/// @param corners  Locations to paint shapes
/// @param circle   Shape to paint
/// @param size     Image size to be painted in to
/// @return			Image with shapes painted in to it.
///@ingroup gRepeatability
Image<bool> paint_circles(const vector<ImageRef>& corners, const vector<ImageRef>& circle, ImageRef size)
{	
	Image<bool> im(size, 0);


	for(unsigned int i=0; i < corners.size(); i++)
		for(unsigned int j=0; j < circle.size(); j++)
			if(im.in_image(corners[i] + circle[j]))
				im[corners[i] + circle[j]] = 1;

	return im;
}

///Computes repeatability the quick way, by caching, but has small rounding errors. This 
///function paints a disc of <code>true</code> around each detected corner in to an image. 
///If a corner warps to a pixel which has the value <code>true</code> then it is a repeat.
///
/// @param warps    Every warping where warps[i][j] specifies warp from image i to image j.
/// @param corners  Detected corners
/// @param r		A corner must be as close as this to be considered repeated
/// @param size		Size of the region for cacheing. All images must be this size.
/// @return 		The repeatability.
/// @ingroup gRepeatability
float compute_repeatability(const vector<vector<Image<Vector<2> > > >& warps, const vector<vector<ImageRef> >& corners, int r, ImageRef size)
{
	unsigned int n = corners.size();

	vector<ImageRef> disc = generate_disc(r);

	vector<Image<bool> >  detected;
	for(unsigned int i=0; i < n; i++)
		detected.push_back(paint_circles(corners[i], disc, size));
	
	int corners_tested = 0;
	int good_corners = 0;

	for(unsigned int i=0; i < n; i++)
		for(unsigned int j=0; j < n; j++)
		{
			if(i==j)
				continue;
			
			for(unsigned int k=0; k < corners[i].size(); k++)
			{	
				ImageRef dest = ir_rounded(warps[i][j][corners[i][k]]);

				if(dest.x != -1)
				{
					corners_tested++;
					if(detected[j][dest])
						good_corners++;
				}
			}
		}
	
	return 1.0 * good_corners / (DBL_EPSILON + corners_tested);
}



/**Load warps from a repeatability dataset. 


The dataset contains warps which round to outside the image by one pixel in the max direction.
  Fast repeatability testing founds values to integers, which causes errors, so these points need
  to be pruned from the dataset. Slow repeatability testing does not do this, and these values must
  be left so that the same data _exactly_ are used for the testing as int FAST-9 paper.

@param stub  The directory of the whole dataset.
@param num   The image numbers for which warps will be loaded.
@param size  The size of the corresponding images.
@param prune Whether to prune points which warp to outside the images.
@return  <code>return_value[i][j][y][x]</code> is where pixel x, y in image i warps to in image j.
@ingroup gRepeatability
*/
vector<vector<Image<Vector<2> > > > load_warps(string stub, const vector<int>& num, ImageRef size, bool prune=1)
{
	stub += "/warps/";

	vector<vector<Image<Vector<2> > > > ret(num.size(), vector<Image<Vector<2> > >(num.size()));

	BasicImage<byte> tester(NULL, size);

	Vector<2> outside = (make_Vector, -1, -1);

	for(unsigned int from = 0; from < num.size(); from ++)
		for(unsigned int to = 0; to < num.size(); to ++)
			if(from != to)
			{
				Image<Vector<2> > w(size, (make_Vector, -1, -1));
				int n = size.x * size.y;
				Image<Vector<2> >::iterator p = w.begin();

				ifstream f;
				string fname = stub + sPrintf("warp_%i_%i.warp", num[from], num[to]);
				f.open(fname.c_str());

				if(!f.good())
				{
					cerr << "Error: " << fname << ": " << strerror(errno) << endl;
					exit(1);
				}

				for(int i=0; i < n; ++i, ++p)
				{
					f >> *p;
					if(prune && !tester.in_image(ir_rounded(*p)))
						*p = outside;
				}
				
				if(!f.good())
				{
					cerr << "Error: " << fname << " went bad" << endl;
					exit(1);
				}

				cerr << "Loaded " << fname << endl;

				ret[from][to] = w;
			}

	return ret;
}

///Load images from a dataset
///@param stub  The directory of the whole dataset.
///@param num   The image numbers for which warps will be loaded.
///@return The loaded images.
///@ingroup gRepeatability
vector<Image<byte> > load_images(string stub, const vector<int>& num)
{
	stub += "/frames/";

	vector<Image<byte> > r;

	for(unsigned int i=0; i < num.size(); i++)
		r.push_back(img_load(stub + sPrintf("frame_%i.pgm", num[i])));
	
	return r;
}



/// Generate a random tree.
///
/// @param s Does nothing
/// @param d Depth of tree to generate
/// @param is_eq_branch Whether eq-branch constraints should be applied. This should
///                     always be true when the function is called.
/// @ingroup gTree
tree_element* random_tree(double s, int d, bool is_eq_branch=1)
{
	//Recursively generate a tree of depth d
	//
	//Generated trees respect invariant 1
	if(d== 0)
		if(is_eq_branch)
			return new tree_element(0);
		else
			return new tree_element(rand()%2);
	else
		return new tree_element(random_tree(s, d-1, 0), random_tree(s, d-1, 1), random_tree(s, d-1, 0), rand()%num_offsets );
}



///Compute the current temperature from parameters in the 
///configuration file.
///
///@ingroup gOptimize
///@param i The current iteration.
///@param imax The maximum number of iterations.
///@return The temperature.
double compute_temperature(int i, int imax)
{
	double scale=GV3::get<double>("Temperature.expo.scale");
	double alpha = GV3::get<double>("Temperature.expo.alpha");

	return scale * exp(-alpha * i / imax);
}




///Generate an optimized corner detector.
///
///@ingroup gOptimize
///@param images The training images
///@param warps  Warps for evaluating the performance on the training images.
///@return An optimized detector.
tree_element* learn_detector(const vector<Image<byte> >& images, const vector<vector<Image<Vector<2> > > >& warps)
{
	unsigned int  iterations=GV3::get<unsigned int>("iterations");
	int threshold = GV3::get<int>("FAST_threshold");
	int fuzz_radius=GV3::get<int>("fuzz");
	double repeatability_scale = GV3::get<double>("repeatability_scale");
	double num_cost	=	GV3::get<double>("num_cost");
	int max_nodes = GV3::get<int>("max_nodes");

	double offset_sigma = GV3::get<double>("offset_sigma");
	


	bool first_time = 1;
	double old_cost = HUGE_VAL;

	ImageRef image_size = images[0].size();

	set<int> debug_triggers = GV3::get<set<int> >("triggers");
	
	//Preallocated space for nonmax-suppression. See tree_detect_corners()
	Image<int> scratch_scores(image_size, 0);

	//Start with an initial random tree
	tree_element* tree = random_tree(offset_sigma, GV3::get<int>("initial_tree_depth"));
	
	for(unsigned int itnum=0; itnum < iterations; itnum++)
	{
		if(debug_triggers.count(itnum))
			GUI.ParseLine(GV3::get<string>(sPrintf("trigger.%i", itnum)));

		/* Trees:

			Invariants:
				1:     eq->{0,0,0,(0,0),0}			//Leafs of an eq pointer must not be corners

			Operations:
				Leaves:
					1: Splat on a random subtree of depth 1 (respect invariant 1)
					2: Flip  class   (respect invariant 1)

				Nodes:
					3: Copy one subtree to another subtree (no invariants need be respected)
					4: Randomize offset (no invariants need be respected)

			Costs:
				Tree size: should respect operation 4 and reduce node count accordingly
		
		*/

		//Cost:
		//
		//  (1 + (#nodes/max_nodes)^2) * (1 - repeatability)^2 * Sum_{frames} exp(- (fast_9_num-detected_num)^2/2sigma^2)
		// 


		//Copy the new tree and work with the copy.
		tree_element* new_tree = tree->copy();

		cout << "\n\n-------------------------------------\n";
		cout << print << "Iteration" << itnum;

		if(GV3::get<bool>("debug.print_old_tree"))
		{
			cout << "Old tree is:" << endl;
			tree->print(cout);
		}
	
		//Skip tree modification first time so that the randomly generated
		//initial tree can be evaluated
		if(!first_time)
		{

			//Create a tree permutation
			tree_element* node;
			bool node_is_eq;

			int nnum = rand() % new_tree->num_nodes();

			cout << "Permuting tree at node " << nnum << endl;


			rpair(node, node_is_eq) = new_tree->nth_element(nnum);

			cout << print << "Node" << node << node_is_eq;

			if(node->eq == NULL) //A leaf
			{
				if(rand() % 2 || node_is_eq)  //Operation 1, invariant 1
				{
					cout << "Growing a subtree:\n";
					//Grow a subtree
					tree_element* stub = random_tree(offset_sigma, 1);

					stub->print(cout);

					//Splice it on manually (ick)
					*node = *stub;
					stub->lt = stub->eq = stub->gt = 0;
					delete stub;

				}
				else //Operation 2
				{
					cout << "Flipping the classification\n";
					node->is_corner  = ! node->is_corner;
				}
			}
			else //A node
			{
				double d = rand_u();

				if(d < 1./3.) //Randomize the test
				{
					cout << "Randomizing the test\n";
					node->offset_index = rand() % num_offsets;
				}
				else if(d < 2./3.)
				{
					int r = rand() % 3; //Remove
					int c;				//Copy
					while((c = rand()%3) == r);

					cout << "Copying branches " << c << " to " << r <<endl;


					tree_element* tmp;

					if(c == 0)
						tmp = node->lt->copy();
					else if(c == 1)
						tmp = node->eq->copy();
					else
						tmp = node->gt->copy();
					
					if(r == 0)
					{
						delete node->lt;
						node->lt = tmp;
					}
					else if(r == 1)
					{
						delete node->eq;
						node->eq = tmp;
					}
					else 
					{
						delete node->gt;
						node->gt = tmp;
					}
				}
				else //Splat!!! ie delete a subtree
				{
					cout << "Splat!!!1\n";
					delete node->lt;
					delete node->eq;
					delete node->gt;
					node->lt = node->eq = node->gt = 0;

					if(node_is_eq) //Maintain invariant 1
						node->is_corner = 0;
					else
						node->is_corner = rand()%2;
				}
			}
		}
		first_time=0;

		if(GV3::get<bool>("debug.print_new_tree"))
		{
			cout << "New tree is: "<< endl;
			new_tree->print(cout);

		}
	
		
		//Detect all corners
		vector<vector<ImageRef> > detected_corners;
		for(unsigned int i=0; i < images.size(); i++)
			detected_corners.push_back(tree_detect_corners(images[i], new_tree, threshold, scratch_scores));


		//Compute repeatability
		double repeatability = compute_repeatability(warps, detected_corners, fuzz_radius, image_size);
		double repeatability_cost = 1 + sq(repeatability_scale/repeatability);

		//Compute cost associated with the total number of detected corners.
		float number_cost=0;
		for(unsigned int i=0; i < detected_corners.size(); i++)
		{
			double cost = sq(detected_corners[i].size() / num_cost);
			cout << print << "Image" << i << detected_corners[i].size()<< cost;
			number_cost += cost;
		}
		number_cost = 1 + number_cost / detected_corners.size();
		cout << print << "Number cost" << number_cost;

		//Cost associated with tree size
		double size_cost = 1 + sq(1.0 * new_tree->num_nodes()/max_nodes);
		
		//The overall cost function
		double cost = size_cost * repeatability_cost * number_cost;

		double temperature = compute_temperature(itnum,iterations);
		

		//The Boltzmann acceptance criterion:
		//If cost < old cost, then old_cost - cost > 0
		//so exp(.) > 1
		//so drand48() < exp(.) == 1
		double liklihood=exp((old_cost-cost) / temperature);

		
		cout << print << "Temperature" << temperature;
		cout << print << "Number cost" << number_cost;
		cout << print << "Repeatability" << repeatability << repeatability_cost;
		cout << print << "Nodes" << new_tree->num_nodes() << size_cost;
		cout << print << "Cost" << cost;
		cout << print << "Old cost" << old_cost;
		cout << print << "Liklihood" << liklihood;
		

		if(rand_u() < liklihood)
		{
			cout << "Keeping change" << endl;
			old_cost = cost;
			delete tree;
			tree = new_tree;
		}
		else
		{
			cout << "Rejecting change" << endl;
			delete new_tree;
		}
		
		cout << print << "Final cost" << old_cost;

	}


	return tree;
}


//Generate a detector, and compute its repeatability for all the tests.
//
//@param argc Number of command line arguments
//@ingroup gRepeatability
void mmain(int argc, char** argv)
{
	GUI.LoadFile("learn_detector.cfg");
	GUI.parseArguments(argc, argv);
	
	string dir=GV3::get<string>("repeatability_dataset.directory");
	vector<int> nums=GV3::get<vector<int> >("repeatability_dataset.examples");

	if(GV3::get<int>("random_seed") != -1)
		srand(GV3::get<int>("random_seed"));

	
	create_offsets();
	draw_offsets();

	vector<Image<byte> > images = load_images(dir, nums);
	vector<vector<Image<Vector<2> > > > warps = load_warps(dir, nums, images.at(0).size());
	tree_element* tree = learn_detector(images, warps);

	cout << "Final tree is:" << endl;
	tree->print(cout);
	cout << endl;

	cout << "Final block detector is:" << endl;
	{
		block_bytecode f = tree->make_fast_detector(9999);
		f.print(cout, 9999);
	}
}


int main(int argc, char** argv)
{
	try
	{	
		mmain(argc, argv);
	}
	catch(Exceptions::All w)
	{	
		cerr << "Error: " << w.what << endl;
	}
}






