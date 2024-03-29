
#include "dataset.hpp"
#include "calib.hpp"
#include "clif_cv.hpp"
#include "enumtypes.hpp"
#include "preproc.hpp"

#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#ifdef CLIF_WITH_HDMARKER
  #include <hdmarker/hdmarker.hpp>
  #include <hdmarker/subpattern.hpp>
  
  using namespace hdmarker;
#endif
  
#ifdef CLIF_WITH_UCALIB
  #include <ucalib/corr_lines.hpp>
#endif
  
#include "mat.hpp"
  
using namespace std;
using namespace cv;

namespace clif {
  
typedef unsigned int uint;

bool pattern_detect(Dataset *s, cpath imgset, cpath calibset, bool write_debug_imgs)
{
  cpath img_root, map_root;
  if (!s->deriveGroup("calibration/imgs", imgset, "calibration/mapping", calibset, img_root, map_root))
    abort();
  
  Datastore *debug_store = NULL;

  CalibPattern pattern;
  s->getEnum(img_root/"type", pattern);
  
  if (write_debug_imgs && pattern == CalibPattern::HDMARKER)
    debug_store = s->getStore(map_root / "data");
  else
    write_debug_imgs = false;
    
  //FIXME implement generic "format" class?
  Datastore *imgs = s->getStore(img_root/"data");
    
  vector<vector<Point2f>> ipoints;
  vector<vector<Point2f>> wpoints;
  
  int channels = imgs->imgChannels();
  if (imgs->org() == DataOrg::BAYER_2x2)
    channels = 3;
  
  //FIXME implement multi-channel calib for opencv checkerboard
  if (pattern == CalibPattern::CHECKERBOARD)
    channels = 1;
  
  Mat_<std::vector<Point2f>> wpoints_m(Idx{channels, imgs->imgCount()});
  Mat_<std::vector<Point2f>> ipoints_m(Idx{channels, imgs->imgCount()});
  
  if (pattern == CalibPattern::CHECKERBOARD) {
    cv::Mat img;
    int size[2];
    
    s->get(img_root / "size", size, 2);
    
    assert(imgs);
    assert(imgs->dims() == 4);
  
    for(int j=0;j<imgs->imgCount();j++) {
      vector<Point2f> corners;
      std::vector<int> idx(4, 0);
      idx[3] = j;
      imgs->readImage(idx, &img, Improc::CVT_8U | Improc::CVT_GRAY | Improc::DEMOSAIC);
      
      cv::Mat ch = clifMat_channel(img, 0);
      
      int succ = findChessboardCorners(ch, Size(size[0],size[1]), corners, CV_CALIB_CB_ADAPTIVE_THRESH+CV_CALIB_CB_NORMALIZE_IMAGE+CALIB_CB_FAST_CHECK+CV_CALIB_CB_FILTER_QUADS);
      
      if (succ) {
        printf("found %6lu corners (img %d/%d)\n", corners.size(), j, imgs->imgCount());
        cornerSubPix(ch, corners, Size(8,8), Size(-1,-1), TermCriteria(cv::TermCriteria::MAX_ITER | cv::TermCriteria::EPS,100,0.0001));
      }
      else
        printf("found      0 corners (img %d/%d)\n", j, imgs->imgCount());
      
      ipoints.push_back(std::vector<Point2f>());
      wpoints.push_back(std::vector<Point2f>());
      
      if (succ) {
        for(int y=0;y<size[1];y++)
          for(int x=0;x<size[0];x++) {
            ipoints.back().push_back(corners[y*size[0]+x]);
            wpoints.back().push_back(Point2f(x,y));
            //pointcount++;
          }
          
        wpoints_m(0, j) = wpoints.back();
        ipoints_m(0, j) = ipoints.back();
      }
    }
  }
#ifdef CLIF_WITH_HDMARKER
  else if (pattern == CalibPattern::HDMARKER) {
    
    double unit_size; //marker size in mm
    double unit_size_res;
    int recursion_depth;
    
    s->get(img_root / "marker_size", unit_size);
    s->get(img_root / "hdmarker_recursion", recursion_depth);
    
    //FIXME remove this!
    Marker::init();
    
    
    assert(imgs);
    
    //FIXME range!
    
    assert(imgs->dims() == 4);
    
    for(int j=0;j<imgs->imgCount();j++) {
      std::vector<Corner> corners_rough;
      std::vector<Corner> corners;
      std::vector<int> idx(4, 0);
      idx[3] = j;
      bool *mask_ptr = NULL;
      bool masks[3][4];
      
      cv::Mat *debug_img_ptr = NULL;
      cv::Mat debug_img;          
      
      if (imgs->org() == DataOrg::BAYER_2x2) {
        for(int mc=0;mc<3;mc++)
          for(int m=0;m<4;m++)
            masks[mc][m] = false;
          
          switch (imgs->order()) {
            case DataOrder::RGGB : 
              masks[0][0] = true;
              masks[1][1] = true;
              masks[1][2] = true;
              masks[2][3] = true;
              break;
            default :
              abort();
          }
          
          
          cv::Mat debug_imgs[3];
          
          //grayscale rough detection
          //FIXME move this up - mmapped reallocation not possible...
          cv::Mat img;
          imgs->readImage(idx, &img, Improc::CVT_8U | Improc::CVT_GRAY | Improc::DEMOSAIC);
          cv::Mat gray = clifMat_channel(img, 0);
          Marker::detect(gray, corners_rough);
          
          cv::Mat bayer_img;
          imgs->readImage(idx, &bayer_img, CVT_8U);
          cv::Mat bayer = clifMat_channel(bayer_img, 0);
          
          char buf[128];
          
          //sprintf(buf, "orig_img%03d.tif", j);
          //imwrite(buf, bayer);
          
          for(int c=0;c<3;c++) {
            if (debug_store)
              debug_img_ptr = &debug_imgs[c];
            
            unit_size_res = unit_size;
            mask_ptr = &masks[c][0];
            hdmarker_detect_subpattern(bayer, corners_rough, corners, recursion_depth, &unit_size_res, debug_img_ptr, mask_ptr, 0);
            
            printf("found %6lu corners for channel %d (img %d/%d)\n", corners.size(), c, j, imgs->imgCount());
            
            //sprintf(buf, "debug_img%03d_ch%d.tif", j, c);
            //imwrite(buf, *debug_img_ptr);
            
            std::vector<Point2f> ipoints_v(corners.size());
            std::vector<Point2f> wpoints_v(corners.size());
            
            for(int ci=0;ci<corners.size();ci++) {
              //FIXME multi-channel calib!
              ipoints_v[ci] = corners[ci].p;
              Point2f w_2d = unit_size_res*Point2f(corners[ci].id.x, corners[ci].id.y);
              wpoints_v[ci] = Point2f(w_2d.x, w_2d.y);
            }
            
            wpoints_m(c, j) = wpoints_v;
            ipoints_m(c, j) = ipoints_v;
            s->flush();
          }
          
          if (debug_store)
            cv::merge(debug_imgs, 3, debug_img);
      }
      else {
        cv::Mat debug_imgs[imgs->imgChannels()];
        
        //grayscale rough detection
        //FIXME move this up - mmapped reallocation not possible...
        cv::Mat img;
        imgs->readImage(idx, &img, Improc::CVT_8U | Improc::CVT_GRAY | Improc::DEMOSAIC);
        cv::Mat gray = clifMat_channel(img, 0);
        Marker::detect(gray, corners_rough);
        
        cv::Mat img_color;
        imgs->readImage(idx, &img_color, CVT_8U);
        
        
        for(int c=0;c<imgs->imgChannels();c++) {
          if (debug_store)
            debug_img_ptr = &debug_imgs[c];
          
          cv::Mat ch = clifMat_channel(img_color, 0);
          
          unit_size_res = unit_size;
          hdmarker_detect_subpattern(ch, corners_rough, corners, recursion_depth, &unit_size_res, debug_img_ptr);
          
          printf("found %6lu corners for channel %d (img %d/%d)\n", corners.size(), c, j, imgs->imgCount());
          
          //char buf[128];
          //sprintf(buf, "debug_img%03d_ch%d.tif", j, c);
          //imwrite(buf, *debug_img_ptr);
          
          std::vector<Point2f> ipoints_v(corners.size());
          std::vector<Point2f> wpoints_v(corners.size());
          
          for(int ci=0;ci<corners.size();ci++) {
            //FIXME multi-channel calib!
            ipoints_v[ci] = corners[ci].p;
            Point2f w_2d = unit_size_res*Point2f(corners[ci].id.x, corners[ci].id.y);
            wpoints_v[ci] = Point2f(w_2d.x, w_2d.y);
          }
          
          wpoints_m(c, j) = wpoints_v;
          ipoints_m(c, j) = ipoints_v;
          s->flush();
        }
        
        if (debug_store) {
          if (imgs->imgChannels() == 1)
            debug_img = debug_imgs[0];
          else
            cv::merge(debug_imgs, imgs->imgChannels(), debug_img);
        }
      }
      
      if (debug_store) {
        debug_store->appendImage(&debug_img);
        s->flush();
        
        //char buf[128];
        //sprintf(buf, "col_fit_img%03d.tif", j);
        //imwrite(buf, debug_img);
      }
    }
  }
  #endif
  else
    abort();
  
  s->setAttribute(map_root / "img_points", ipoints_m);
  s->setAttribute(map_root / "world_points", wpoints_m);
  
  std::vector<int> imgsize = { imgSize(imgs).width, imgSize(imgs).height };
  
  s->setAttribute(map_root / "img_size", imgsize);
  
  return false;
}

bool opencv_calibrate(Dataset *set, int flags, cpath map, cpath calib)
{
  cpath map_root, calib_root;
  if (!set->deriveGroup("calibration/mapping", map, "calibration/intrinsics", calib, map_root, calib_root))
    abort();
  
  cv::Mat cam;
  vector<double> dist;
  vector<cv::Mat> rvecs;
  vector<cv::Mat> tvecs;
  int im_size[2];
  
  Mat_<std::vector<Point2f>> wpoints_m;
  Mat_<std::vector<Point2f>> ipoints_m;
    
  vector<vector<Point2f>> ipoints;
  vector<vector<Point3f>> wpoints;
  
  Attribute *w_a = set->get(map_root/"world_points");
  Attribute *i_a = set->get(map_root/"img_points");
  set->get(map_root/"img_size", im_size, 2);
  
  //FIXME error handling
  if (!w_a || !i_a)
    abort();
  
  w_a->get(wpoints_m);
  i_a->get(ipoints_m);
  
  for(int i=0;i<wpoints_m[1];i++) {
    if (!wpoints_m(0, i).size())
      continue;
    
    ipoints.push_back(ipoints_m(0, i));
    wpoints.push_back(std::vector<Point3f>(wpoints_m(0, i).size()));
    for(int j=0;j<wpoints_m(0, i).size();j++)
      wpoints.back()[j] = Point3f(wpoints_m(0, i)[j].x,wpoints_m(0, i)[j].y,0);
  }
  
  double rms = calibrateCamera(wpoints, ipoints, cv::Size(im_size[0],im_size[1]), cam, dist, rvecs, tvecs, flags);
  
  
  printf("opencv calibration rms %f\n", rms);
  
  std::cout << cam << std::endl;
  
  double f[2] = { cam.at<double>(0,0), cam.at<double>(1,1) };
  double c[2] = { cam.at<double>(0,2), cam.at<double>(1,2) };
  
  //FIXME todo delete previous group?!
  set->setAttribute(calib_root / "type", "CV8");
  set->setAttribute(calib_root / "projection", f, 2);
  set->setAttribute(calib_root / "projection_center", c, 2);
  set->setAttribute(calib_root / "opencv_distortion", dist);
  
  return true;
}
  
  //FIXME repair!
  bool ucalib_calibrate(Dataset *set, cpath proxy, cpath calib)
#ifdef CLIF_WITH_UCALIB
  {
    cpath proxy_root, calib_root;
    if (!set->deriveGroup("calibration/proxy", proxy, "calibration/intrinsics", calib, proxy_root, calib_root))
      abort();
    
    int im_size[2];
      
    vector<vector<Point2f>> ipoints;
    vector<vector<Point3f>> wpoints;
    
    Mat_<float> proxy_m;
    Mat_<float> corr_line_m;
    
    set->get(proxy_root/"source/img_size", im_size, 2);
    
    Cam_Config cam_config = { 0.0065, 12.0, 300.0, -1, -1 };
    Calib_Config conf = { true, 190, 420 };

    cam_config.w = im_size[0];
    cam_config.h = im_size[1];
      
    Datastore *proxy_store = set->store(proxy_root/"proxy");
    proxy_store->read(proxy_m);
    
    corr_line_m.create(Idx({4, proxy_m[1],proxy_m[2], proxy_m[3]}));
    
    Point2i proxy_size(proxy_m[1],proxy_m[2]);
    
    for(int color=0;color<proxy_m[3];color++) {
      DistCorrLines dist_lines = DistCorrLines(0, 0, 0, cam_config.w, cam_config.h, 100.0, cam_config, conf, proxy_size);
      dist_lines.proxy_backwards.resize(proxy_m[4]);
      
      Idx pos(proxy_store->dims());
      assert(proxy_store->dims() == 5);
      
      for(int img_n=0;img_n<proxy_m[4];img_n++) {
        dist_lines.proxy_backwards[img_n].resize(proxy_size.y*proxy_size.x);
        for(int j=0;j<proxy_size.y;j++)
          for(int i=0;i<proxy_size.x;i++) {
            dist_lines.proxy_backwards[img_n][j*proxy_size.x+i].x = proxy_m(0, i, j, color, img_n);
            dist_lines.proxy_backwards[img_n][j*proxy_size.x+i].y = proxy_m(1, i, j, color, img_n);
          }
      }
      
      dist_lines.proxy_fit_lines_global();
      char buf[64];
      sprintf(buf, "center%02d", color);
      dist_lines.Draw(buf);
      
      for(int j=0;j<proxy_size.y;j++)
        for(int i=0;i<proxy_size.x;i++) {
          corr_line_m(0, i, j, color) = dist_lines.linefits[j*proxy_size.y+i][0];
          corr_line_m(1, i, j, color) = dist_lines.linefits[j*proxy_size.y+i][1];
          corr_line_m(2, i, j, color) = dist_lines.linefits[j*proxy_size.y+i][2];
          corr_line_m(3, i, j, color) = dist_lines.linefits[j*proxy_size.y+i][3];
        }
    }
    
    Datastore *line_store = set->addStore(calib_root/"lines");
    line_store->write(proxy_m);
    
    set->setAttribute(calib_root/"type", "ucalib");
    
    return true;
  }
#else
  {
    //FIXME report error
    return false;
  }
#endif
  
bool generate_proxy_loess(Dataset *set, int proxy_w, int proxy_h , cpath map, cpath proxy)
#ifdef CLIF_WITH_UCALIB
{
  cpath map_root, proxy_root;
  if (!set->deriveGroup("calibration/mapping", map, "calibration/proxy", proxy, map_root, proxy_root))
    abort();
  
  cv::Mat cam;
  vector<double> dist;
  vector<cv::Mat> rvecs;
  vector<cv::Mat> tvecs;
  int im_size[2];
  
  Mat_<std::vector<Point2f>> wpoints_m;
  Mat_<std::vector<Point2f>> ipoints_m;
  
  Mat_<float> proxy_m;
  
  //2-d (color, imgs)
  Attribute *w_a = set->get(map_root/"world_points");
  Attribute *i_a = set->get(map_root/"img_points");
  set->get(map_root/"img_size", im_size, 2);
  
  
  //FIXME error handling
  if (!w_a || !i_a)
    abort();
  
  w_a->get(wpoints_m);
  i_a->get(ipoints_m);
  
  proxy_m.create(Idx({2, proxy_w, proxy_h, wpoints_m[0], wpoints_m[1]}));
  
  Cam_Config cam_config = { 0.0065, 12.0, 300.0, im_size[0], im_size[1] };
  Calib_Config conf = { true, 190, 420 };
  
  for(int color=0;color<wpoints_m[0];color++) {
    vector<vector<Point2f>> ipoints;
    vector<vector<Point3f>> wpoints;
    
    for(int i=0;i<wpoints_m[1];i++) {
      if (!wpoints_m(color, i).size())
        continue;
      
      ipoints.push_back(ipoints_m(color, i));
      wpoints.push_back(std::vector<Point3f>(wpoints_m(color, i).size()));
      for(int j=0;j<wpoints_m(color, i).size();j++) {
        wpoints.back()[j] = Point3f(wpoints_m(color, i)[j].x,wpoints_m(color, i)[j].y,0);
      }
    }
    
    Point2i proxy_size(proxy_w,proxy_h);
    
    DistCorrLines dist_lines = DistCorrLines(0, 0, 0, cam_config.w, cam_config.h, 100.0, cam_config, conf, proxy_size);
    dist_lines.add(ipoints, wpoints, 20.0);
    dist_lines.proxy_backwards_poly_generate();
    
    for(int img_n=0;img_n<ipoints.size();img_n++)
      for(int j=0;j<proxy_size.y;j++)
        for(int i=0;i<proxy_size.x;i++) {
          proxy_m(0, i, j, color, img_n) = dist_lines.proxy_backwards[img_n][j*proxy_size.x+i].x;
          proxy_m(1, i, j, color, img_n) = dist_lines.proxy_backwards[img_n][j*proxy_size.x+i].y;
        }
      
    /*cv::Mat proxy_img;
    proxy_img.create(Size(proxy_size.x, proxy_size.y), CV_32FC2);
    Datastore *proxy_store = set->addStore(proxy_root/"proxy");
    proxy_store->setDims(4);
    
    for(int img_n=0;img_n<ipoints.size();img_n++) {
      for(int j=0;j<proxy_size.y;j++)
        for(int i=0;i<proxy_size.x;i++) {
          proxy_img.at<Point2f>(j,i) = dist_lines.proxy_backwards[img_n][j*proxy_size.x+i];
        }
        proxy_store->appendImage(&proxy_img);
    }*/
  }
  
  Datastore *proxy_store = set->addStore(proxy_root/"proxy");
  proxy_store->write(proxy_m);
  
  return true;
}
#else
{
  //FIXME report error
  return false;
}
#endif

}
