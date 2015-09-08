#include "clif3dsubset.hpp"

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"

using namespace std;
using namespace clif;
using namespace clif_cv;
using namespace cv;

Clif3DSubset::Clif3DSubset(ClifDataset *data, std::string extr_group)
: _data(data)
{
  string root("calibration/extrinsics/");
  root = root.append(extr_group);
  
  ExtrType type;
  
  data->getEnum((root+"/type").c_str(), type);
  
  assert(type == ExtrType::LINE);
  
  double line_step[3];
  
  data->getAttribute(root+"/line_step", line_step, 3);
  
  //TODO for now we only support horizontal lines!
  assert(line_step[0] != 0.0);
  assert(line_step[1] == 0.0);
  assert(line_step[2] == 0.0);

  step_length = line_step[0];
  
  //TODO which intrinsic to select!
  
  vector<string> intrs;
  data->listSubGroups("calibration/intrinsics", intrs);
  
  data->getAttribute("calibration/intrinsics/"+intrs[0]+"/projection", f, 2);
}


typedef Vec<ushort, 3> Vec3us;

template<typename V> void warp_1d_linear_int(Mat in, Mat out, double offset)
{
  
  if (abs(offset) >= in.size().width) {
    return;
  }
  
  V *in_ptr = in.ptr<V>(0);
  V *out_ptr = out.ptr<V>(0);
  
  int off_i;
  int f1, f2;
  
  if (offset >= 0)
    off_i = offset;
  else
    off_i = offset-1;
  
  f1 = (offset - (double)off_i)*1024;
  f2 = 1024 - f1;
  
  for(int i=clamp<int>(off_i, 1, out.size().width-1);i<clamp<int>(out.size().width+off_i, 1, out.size().width-1);i++) {
    out_ptr[i] = (f1*(Vec3i)in_ptr[i-off_i] + f2*(Vec3i)in_ptr[i-off_i+1])/1024;
  }
  /*for(int i=0;i<out.size().width;i++) {
    out_ptr[i] = in_ptr[i];
  }*/
}


void Clif3DSubset::readEPI(cv::Mat &m, int line, double depth, int flags, int interp)
{      
  double step = f[0]*step_length/depth;
  
  cv::Mat tmp;
  readCvMat(_data, 0, tmp, flags | CLIF_DEMOSAIC);

  m = cv::Mat::zeros(cv::Size(imgSize(_data).width, _data->imgCount()), tmp.type());
  
  for(int i=0;i<_data->imgCount();i++)
  {      
    //FIXME rounding?
    double d = step*(i-_data->imgCount()/2);
    
    if (abs(d) >= tmp.size().width)
      continue;
    
    readCvMat(_data, i, tmp, flags | CLIF_DEMOSAIC);
    
    if (tmp.type() == CV_16UC3)
      warp_1d_linear_int<Vec3us>(tmp.row(line), m.row(i), d);
    else if (tmp.type() == CV_8UC3)
      warp_1d_linear_int<Vec3b>(tmp.row(line), m.row(i), d);
    //Matx23d warp(1,0,d, 0,1,0);
    //warpAffine(tmp.row(line), m.row(i), warp, m.row(i).size(), CV_INTER_LANCZOS4);
  }
}