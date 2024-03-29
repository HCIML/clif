#include "cliini.h"

#include <cstdlib>
#include <vector>

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE //FIXME need portable extension matcher...
#endif
#include <fnmatch.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef CLIF_COMPILER_MSVC
#include <unistd.h>
#endif


#include "dataset.hpp"
#include "hdf5.hpp"
#include "clif_cv.hpp"
#include "calib.hpp"

#include "opencv2/highgui/highgui.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "opencv2/imgproc/imgproc.hpp"

using namespace clif;
using namespace std;
using namespace cv;

cliini_opt opts[] = {
  {
    "help",
    0, //argcount
    0, //argcount
    CLIINI_NONE, //type
    0, //flags
    'h'
  },
  {
    "input",
    1, //argcount min
    CLIINI_ARGCOUNT_ANY, //argcount max
    CLIINI_STRING, //type
    0, //flags
    'i'
  },
  {
    "output",
    1, //argcount
    CLIINI_ARGCOUNT_ANY, //argcount
    CLIINI_STRING, //type
    0, //flags
    'o'
  },
  {
    "types", //FIXME remove this
    1, //argcount
    1, //argcount
    CLIINI_STRING, //type
    0, //flags
    't'
  },
  {
    "store",
    2, //argcount
    CLIINI_ARGCOUNT_ANY, //argcount
    CLIINI_STRING,
    0,
    's'
  },
  {
    "include",
    1, //argcount
    CLIINI_ARGCOUNT_ANY, //argcount
    CLIINI_STRING
  },
  {
    "exclude",
    1, //argcount
    CLIINI_ARGCOUNT_ANY, //argcount
    CLIINI_STRING
  },
  {
    "detect-patterns",
    0,0
  },
  {
    "opencv-calibrate",
    0,0
  },
  {
    "ucalib-calibrate",
    0,0
  },
  {
    "gen-proxy-loess",
    0,0
  }
};

cliini_optgroup group = {
  opts,
  NULL,
  sizeof(opts)/sizeof(*opts),
  0,
  0
};

const char *clif_extension_pattern = "*.cli?";
const char *ini_extension_pattern = "*.ini";
const char *mat_extension_pattern = "*.mat";
//ksh extension match using FNM_EXTMATCH
const char *img_extension_pattern = "*.+(png|tif|tiff|jpg|jpeg|jpe|jp2|bmp|dib|pbm|pgm|ppm|sr|ras)";

vector<string> extract_matching_strings(cliini_arg *arg, const char *pattern)
{
  vector<string> files;
  
  for(int i=0;i<cliarg_sum(arg);i++)
    //FIXME not working on windows!
    if (!fnmatch(pattern, cliarg_nth_str(arg, i), FNM_CASEFOLD | FNM_EXTMATCH))
      files.push_back(cliarg_nth_str(arg, i));
    
  return files;
}

void errorexit(const char *msg)
{
  printf("ERROR: %s\n",msg);
  exit(EXIT_FAILURE);
}

inline bool file_exists(const std::string& name)
{
  struct stat st;   
  return (stat(name.c_str(), &st) == 0);
}

typedef unsigned int uint;

bool link_output = true;

int main(const int argc, const char *argv[])
{
  bool inplace = false;
  cliini_args *args = cliini_parsopts(argc, argv, &group);

  cliini_arg *input = cliargs_get(args, "input");
  cliini_arg *output = cliargs_get(args, "output");
  cliini_arg *types = cliargs_get(args, "types");
  cliini_arg *calib_imgs = cliargs_get(args, "calib-images");
  cliini_arg *include = cliargs_get(args, "include");
  cliini_arg *exclude = cliargs_get(args, "exclude");
  cliini_arg *stores = cliargs_get(args, "store");
  
  if (!args || cliargs_get(args, "help\n") || (!input && !calib_imgs && !stores) || !output) {
    printf("TODO: print help!");
    return EXIT_FAILURE;
  }
  
  //several modes:                  input      output
  //add/append data to clif file     N x *     1x clif
  //extract image/and or ini        1 x clif  1x init/ Nx img pattern
  
  vector<string> clif_append = extract_matching_strings(output, clif_extension_pattern);
  vector<string> clif_extract_images = extract_matching_strings(output, img_extension_pattern);
  vector<string> clif_extract_attributes = extract_matching_strings(output, ini_extension_pattern);
  
  vector<string> input_clifs = extract_matching_strings(input, clif_extension_pattern);
  vector<string> input_imgs  = extract_matching_strings(input, img_extension_pattern);
  vector<string> input_inis  = extract_matching_strings(input, ini_extension_pattern);
  //vector<string> input_mats  = extract_matching_strings(input, mat_extension_pattern);
  vector<string> input_calib_imgs = extract_matching_strings(calib_imgs, img_extension_pattern);
  
  /*if (input_mats.size()) {
    for(int i=0;i<input_mats.size();i++)
      list_mat(input_mats[i]);
    return EXIT_SUCCESS;
  }*/
  
  bool output_clif;
  //std::string input_set_name;
  std::string output_set_name("default");
  
  if (clif_append.size()) {
    if (clif_append.size() > 1)
      errorexit("only a single output clif file may be specififed!");
    if (clif_extract_images.size() || clif_extract_attributes.size())
      errorexit("may not write to clif at the same time as extracting imgs/attributes!");
    output_clif = true;
  }
  else {
    if (!clif_extract_images.size() && !clif_extract_attributes.size())
      errorexit("no valid output format found!");
    if (input_clifs.size() != 1)
      errorexit("only single input clif file allowed!");
    output_clif = false;
  }
  
  if (output_clif && input_clifs.size() && !clif_append[0].compare(input_clifs[0]))
    inplace = true;
  
  if (!output_clif && stores)
    errorexit("can only add datastores to clif output!");
  
  if (output_clif) {
    ClifFile f_out;
    
    if (file_exists(clif_append[0])) {
      f_out.open(clif_append[0], H5F_ACC_RDWR);
    }
    else
      f_out.create(clif_append[0]);
    
    Dataset *set;
    //FIXME multiple dataset handling!
    if (f_out.datasetCount()) {
      printf("INFO: appending to HDF5 DataSet %s\n", f_out.datasetList()[0].c_str());
      /*if (link_output && input_clifs.size()) {
        printf("FIXME: check if linking into existing dataset works\n");
        set = new Dataset();
      }
      else
        */
      set = f_out.openDataset(0);
    }
    else {
      printf("INFO: creating new HDF5 DataSet %s\n", output_set_name.c_str());
      
      if (link_output && input_clifs.size()) {
        set = new Dataset();
      }
      else
        set = f_out.createDataset(output_set_name);
    }
    //if (!set->file().valid())
      //abort();
    
    if (!inplace)
    for(uint i=0;i<input_clifs.size();i++) {
      ClifFile f_in(input_clifs[i], H5F_ACC_RDONLY);
      
      //FIXME input name handling/selection
      if (f_in.datasetCount() != 1)
        errorexit("FIXME: at the moment only files with a single dataset are supported by this program.");
      
      //FIXME implement dataset handling for datasets without datastore!
      //TODO check: is ^ already working?
      Dataset *in_set = f_in.openDataset(0);
      if (include)
        for(int i=0;i<in_set->Attributes::count();i++) {
          bool match_found = false;
          Attribute *a = in_set->get(i);
          for(int j=0;j<cliarg_sum(include);j++)
            if (!fnmatch(cliarg_nth_str(include, j), a->name.generic_string().c_str(), FNM_PATHNAME)) {
              printf("include attribute: %s\n", a->name.generic_string().c_str());
              match_found = true;
              break;
            }
          if (match_found)
            set->append(a);
        }
      else if (exclude)
        for(int i=0;i<in_set->Attributes::count();i++) {
          bool match_found = false;
          Attribute *a = in_set->get(i);
          for(int j=0;j<cliarg_sum(exclude);j++)
            if (!fnmatch(cliarg_nth_str(exclude, j), a->name.generic_string().c_str(), FNM_PATHNAME)) {
              printf("exclude attribute: %s\n", a->name.c_str());
              match_found = true;
              break;
            }
          if (!match_found)
            set->append(a);
        }
      else {
        if (!link_output)
          set->append(in_set);
        else {
          printf("create dataset by linking!\n");
          set->link(f_out, in_set);
        }
      }
      
      if (link_output && (include || exclude))
        printf("FIXME filtering not yet supported together with linking!\n");
      
      if (!link_output) {
        vector<string> h5datasets = listH5Datasets(f_in.f, in_set->path().generic_string());
        for(uint j=0;j<h5datasets.size();j++) {
          cout << "copy dataset" << h5datasets[j] << endl;
          //FIXME handle dataset selection properly!
          if (!h5_obj_exists(f_out.f, h5datasets[j])) {
            h5_create_path_groups(f_out.f, cpath(h5datasets[j]).parent_path());
            f_out.f.flush(H5F_SCOPE_GLOBAL);
            H5Ocopy(f_in.f.getId(), h5datasets[j].c_str(), f_out.f.getId(), h5datasets[j].c_str(), H5P_DEFAULT, H5P_DEFAULT);
          }
          else
            printf("TODO: dataset %s already exists in output, skipping!\n", h5datasets[j].c_str());
        }
        
        delete set;
        //FIXME dataset idx!
        set = f_out.openDataset(0);
      }
      delete in_set;
    }
    
    for(uint i=0;i<input_inis.size();i++) {
      printf("append ini file!\n");
      Attributes others;
      //FIXME append types (not replace!)
      if (types)
        others.open(input_inis[i].c_str(), cliarg_str(types));
      else
        others.open(input_inis[i].c_str());
      set->append(others);
    }
    
    if (stores) {
      int sum = 0;
      for(uint i=0;i<cliarg_inst_count(stores);i++) {
        int count = cliarg_inst_arg_count(stores, i);
        char *store_path = cliarg_nth_str(stores, sum);
        int dim = atoi(cliarg_nth_str(stores, sum+1));
        sum += 2;
        if (dim < 3) {
          printf("ERROR: datastore %s needs at least 4 dimensions, but dimension was %d (%s)\n", store_path, dim, cliarg_nth_str(stores, sum));
          exit(EXIT_FAILURE);
        }
        printf("add store %s\n", store_path);
        Datastore *store = set->addStore(store_path, dim);
        for(int j=2;j<cliarg_inst_arg_count(stores, i);j++,sum++) {
          printf("%s\n", cliarg_nth_str(stores, sum));
          cv::Mat img = imread(cliarg_nth_str(stores, sum), CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
          if (img.channels() == 3)
            cvtColor(img, img, COLOR_BGR2RGB);
          store->appendImage(&img);
        }
      }
    }
    
    //FIXME allow "empty" datasets with only attributes!
    //FIXME handle appending to datastore!
    //FIXME check wether image format was sufficiently defined!
    //FIXME check wether image format was sufficiently defined!
    //FIXME how do we handle overwriting of data?
    
    //set->writeAttributes();
    
    //set dims for 3d LG (imgs are 3d themselves...)
    
    Datastore *store = set->getStore("data");
    
    for(uint i=0;i<input_imgs.size();i++) {
      printf("store idx %d: %s\n", i, input_imgs[i].c_str());
      cv::Mat img = imread(input_imgs[i], CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
      assert(img.size().width && img.size().height);
      if (img.channels() == 3)
        cvtColor(img, img, COLOR_BGR2RGB);
      store->appendImage(&img);
    }
    
    if (cliargs_get(args, "detect-patterns")) {
      pattern_detect(set);
      //set->writeAttributes();
    }
    if (cliargs_get(args, "gen-proxy-loess")) {
      generate_proxy_loess(set, 33, 25);
      //set->writeAttributes();
    }
    if (cliargs_get(args, "opencv-calibrate")) {
      opencv_calibrate(set);
      //set->writeAttributes();
    }
    if (cliargs_get(args, "ucalib-calibrate")) {
      ucalib_calibrate(set);
      //set->writeAttributes();
    }
    delete set;
  }
  else {
    
    for(uint i=0;i<clif_extract_attributes.size();i++) {
      ClifFile f_in(input_clifs[0], H5F_ACC_RDONLY);
      
      //FIXME input name handling/selection
      if (f_in.datasetCount() != 1)
        errorexit("FIXME: at the moment only files with a single dataset are supported by this program.");
      
      //FIXME implement dataset handling for datasets without datastore!
        Dataset *in_set = f_in.openDataset(0);
        in_set->writeIni(clif_extract_attributes[i]);
        delete in_set;
    }
    
    for(uint i=0;i<clif_extract_images.size();i++) {
      ClifFile f_in(input_clifs[0], H5F_ACC_RDONLY);
      
      //FIXME input name handling/selection
      if (f_in.datasetCount() != 1)
        errorexit("FIXME: at the moment only files with a single dataset are supported by this program.");
      
      Dataset *in_set = f_in.openDataset(0);
      Datastore *in_imgs = in_set->getStore("data");
        
      char buf[4096];
      for(int c=0;c<in_imgs->imgCount();c++) {
        cv::Mat img;
        sprintf(buf, clif_extract_images[i].c_str(), c);
        printf("store idx %d: %s\n", c, buf);
        std::vector<int> idx(in_imgs->dims(), 0);
        idx[3] = c;
        in_imgs->readImage(idx, &img);
        clifMat2cv(&img, &img);
        imwrite(buf, img);
      }
      delete in_set;
    }
  }

  return EXIT_SUCCESS;
}