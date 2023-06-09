#include "wave3d.hpp"
#include "vecmatop.hpp"

#define DVMAX 400

//---------------------------------------------------------------------
int Wave3d::eval(ParVec<int,cpx,PtPrtn>& den, ParVec<int,cpx,PtPrtn>& val)
{
  iC( MPI_Barrier(MPI_COMM_WORLD) );
  _self = this;
  time_t t0, t1;
  int mpirank = this->mpirank();
  int mpisize = this->mpisize();
  double eps = 1e-12;
  double K = this->K();
  ParVec<int, Point3, PtPrtn>& pos = (*_posptr);
  //1. go thrw posptr to get nonlocal points
  vector<int> reqpts;
  for(map<int,Point3>::iterator mi=pos.lclmap().begin(); mi!=pos.lclmap().end(); mi++)
	reqpts.push_back( (*mi).first );
  //2. call den's get
  vector<int> all(1,1);
  iC( den.getBegin(reqpts, all) );  iC( den.getEnd(all) );
  //iC( den.get(reqpts, all) );
  //3. compute extden using ptidxvec
  for(map<BoxKey,BoxDat>::iterator mi=_boxvec.lclmap().begin(); mi!=_boxvec.lclmap().end(); mi++) {
	BoxKey curkey = (*mi).first;
	BoxDat& curdat = (*mi).second;
	if(curdat.tag() & WAVE3D_PTS) {
	  if(_boxvec.prtn().owner(curkey)==mpirank && isterminal(curdat)) {
		vector<int>& curpis = curdat.ptidxvec();
		CpxNumVec& extden = curdat.extden();
		extden.resize(curpis.size());
		for(int k=0; k<curpis.size(); k++) {
		  int poff = curpis[k];
		  extden(k) = den.access(poff);
		}
	  }
	}
  }
  iC( den.discard(reqpts) );
  //3. gather maps, low frequency level by level, high frequency dir by dir
  map< double, vector<BoxKey> > ldmap;
  map< Index3, pair< vector<BoxKey>, vector<BoxKey> > > hdmap;
  for(map<BoxKey,BoxDat>::iterator mi=_boxvec.lclmap().begin(); mi!=_boxvec.lclmap().end(); mi++) {
    BoxKey curkey = (*mi).first;
    BoxDat& curdat = (*mi).second;
    double W = width(curkey);
    if( curdat.tag() & WAVE3D_PTS) {
      if(_boxvec.prtn().owner(curkey)==mpirank) {
	if(W<1-eps) {
	  ldmap[W].push_back(curkey);
	} else {
	  BndDat dummy;
	  for(set<Index3>::iterator si=curdat.outdirset().begin(); si!=curdat.outdirset().end(); si++) {
	    hdmap[*si].first.push_back(curkey);
	    //into bndvec
	    BndKey bndkey;			bndkey.first = curkey;			bndkey.second = *si;
	    _bndvec.insert(bndkey, dummy);
	  }
	  for(set<Index3>::iterator si=curdat.incdirset().begin(); si!=curdat.incdirset().end(); si++) {
	    hdmap[*si].second.push_back(curkey);
	    //into bndvec
	    BndKey bndkey;			bndkey.first = curkey;			bndkey.second = *si;
	    _bndvec.insert(bndkey, dummy);
	  }
	}
      }
    }
  }
  set<BoxKey> reqboxset;
  //LOW UP
  if(mpirank==0) cout<<"LOW UP"<<endl;
  t0 = time(0);
  for(map< double, vector<BoxKey> >::iterator mi=ldmap.begin(); mi!=ldmap.end(); mi++) {
    iC( eval_upward_low((*mi).first, (*mi).second, reqboxset) );
  }
  iC( MPI_Barrier(MPI_COMM_WORLD) );
  t1 = time(0);  if(mpirank==0) {	cout<<"LOW UP "<<difftime(t1,t0)<<"secs "<<endl;  }
  //HIGH
  if(mpirank==0) cout<<"HGH"<<endl;
  t0 = time(0);
  vector<Index3> tmpdirs;
  for(map<Index3, pair< vector<BoxKey>, vector<BoxKey> > >::iterator mi=hdmap.begin(); mi!=hdmap.end(); mi++) {
    Index3 dir = (*mi).first;
    if(dir2width(dir)==1)
      tmpdirs.push_back(dir);
  }
  vector<Index3> basedirs(tmpdirs.size()); //cout<<basedirs.size()<<endl;
  for(int i=0; i<tmpdirs.size(); i++) {
    int off = (i*149)%(tmpdirs.size());	//if(mpirank==0) cout<<off<<endl;
    basedirs[i] = tmpdirs[off];
  }
  int TTL = basedirs.size();
  int DPG = 4;
  int NG = (TTL-1) / DPG + 1;
  if(1) {
    int cur = 0;
    vector<int> mask(BndDat_Number,0);	mask[BndDat_dirupeqnden] = 1;
    //U1
    set<BndKey> reqbndset;
    for(int i=cur*DPG; i<min((cur+1)*DPG,TTL); i++) {
      Index3 dir = basedirs[i];
      iC( eval_upward_hgh_recursive(1, dir, hdmap, reqbndset) );
    }
    //S1
    vector<BndKey> reqbnd;	  reqbnd.insert(reqbnd.begin(), reqbndset.begin(), reqbndset.end());
    iC( _bndvec.getBegin(reqbnd, mask) );
  }
  for(int cur=1; cur<NG; cur++) {
    int pre = cur-1;
    vector<int> mask(BndDat_Number,0);	mask[BndDat_dirupeqnden] = 1;
    //Ucur
    set<BndKey> reqbndset;
    for(int i=cur*DPG; i<min((cur+1)*DPG,TTL); i++) {
      Index3 dir = basedirs[i];
      iC( eval_upward_hgh_recursive(1, dir, hdmap, reqbndset) );
    }
    //Rpre
    iC( _bndvec.getEnd(mask) );
    //Scur
    vector<BndKey> reqbnd;	  reqbnd.insert(reqbnd.begin(), reqbndset.begin(), reqbndset.end());
    iC( _bndvec.getBegin(reqbnd, mask) );
    //Dpre
    for(int i=pre*DPG; i<min((pre+1)*DPG,TTL); i++) {
      Index3 dir = basedirs[i]; //LEXING: PRE HERE
      iC( eval_dnward_hgh_recursive(1, dir, hdmap) );
    }
  }
  if(1) {
    int cur = NG-1;
    vector<int> mask(BndDat_Number,0);	mask[BndDat_dirupeqnden] = 1;
    //Rcur
    iC( _bndvec.getEnd(mask) );
    //Dcur
    for(int i=cur*DPG; i<min((cur+1)*DPG,TTL); i++) {
      Index3 dir = basedirs[i];
      iC( eval_dnward_hgh_recursive(1, dir, hdmap) );
    }
  }
  t1 = time(0);  if(mpirank==0) {	cout<<"HGH "<<difftime(t1,t0)<<"secs "<<endl;  }
  //LOW COMM
  vector<BoxKey> reqbox;  reqbox.insert(reqbox.begin(), reqboxset.begin(), reqboxset.end());
  vector<int> mask(BoxDat_Number,0);
  mask[BoxDat_extden] = 1;  mask[BoxDat_upeqnden] = 1;
  t0 = time(0);
  iC( _boxvec.getBegin(reqbox, mask) );  iC( _boxvec.getEnd(mask) );
  t1 = time(0);  if(mpirank==0) {	cout<<"LOW COMM "<<difftime(t1,t0)<<"secs "<<endl;  }
  //
  //LOW DOWN
  if(mpirank==0) cout<<"LOW DN"<<endl;
  t0 = time(0);
  for(map< double, vector<BoxKey> >::reverse_iterator mi=ldmap.rbegin(); mi!=ldmap.rend(); mi++) {
	iC( eval_dnward_low((*mi).first, (*mi).second) );
  }
  t1 = time(0);  if(mpirank==0) {	cout<<"LOW DN "<<difftime(t1,t0)<<"secs "<<endl;  }
  iC( MPI_Barrier(MPI_COMM_WORLD) );
  //set val from extval
  vector<int> wrtpts;
  for(map<int,Point3>::iterator mi=pos.lclmap().begin(); mi!=pos.lclmap().end(); mi++)
    if( pos.prtn().owner( (*mi).first )!=mpirank ) {
      wrtpts.push_back( (*mi).first );
    }
  val.expand(wrtpts);
  for(map<BoxKey,BoxDat>::iterator mi=_boxvec.lclmap().begin(); mi!=_boxvec.lclmap().end(); mi++) {
    BoxKey curkey = (*mi).first;
    BoxDat& curdat = (*mi).second;
    if(curdat.tag() & WAVE3D_PTS) {
      if(_boxvec.prtn().owner(curkey)==mpirank && isterminal(curdat)==true) {
	CpxNumVec& extval = curdat.extval();
	vector<int>& curpis = curdat.ptidxvec();
	for(int k=0; k<curpis.size(); k++) {
	  int poff = curpis[k];
	  val.access(poff) = extval(k);
	}
      }
    }
  }
  //call val->put
  val.putBegin(wrtpts, all);  val.putEnd(all);
  //val.put(wrtpts, all);
  val.discard(wrtpts);
  iC( MPI_Barrier(MPI_COMM_WORLD) );
  return 0;
}

//---------------------------------------------------------------------
int Wave3d::eval_upward_low(double W, vector<BoxKey>& srcvec, set<BoxKey>& reqboxset)
{
  DblNumMat uep;
  DblNumMat ucp;
  NumVec<CpxNumMat> uc2ue;
  NumTns<CpxNumMat> ue2uc;
  iC( _mlibptr->upward_lowfetch(W, uep, ucp, uc2ue, ue2uc) );
  //---------------
  int sdof = 1;  int tdof = 1;
  for(int k=0; k<srcvec.size(); k++) {
    BoxKey srckey = srcvec[k];
    BoxDat& srcdat = _boxvec.access(srckey);
    if(srcdat.tag() & WAVE3D_PTS) {
      //-----------------------------------------------------------------------------
      Point3 srcctr = center(srckey);
      //get array
      CpxNumVec upchkval(tdof*ucp.n());	  setvalue(upchkval,cpx(0,0));
      CpxNumVec& upeqnden = srcdat.upeqnden();
      //ue2dc
      if(isterminal(srcdat)==true) {
	DblNumMat upchkpos(ucp.m(), ucp.n());
	for(int k=0; k<ucp.n(); k++)
	  for(int d=0; d<dim(); d++)
	    upchkpos(d,k) = ucp(d,k) + srcctr(d);
	//mul
	CpxNumMat mat;		  iC( _knl.kernel(upchkpos, srcdat.extpos(), srcdat.extpos(), mat) );
	iC( zgemv(1.0, mat, srcdat.extden(), 1.0, upchkval) );
      } else {
	for(int a=0; a<2; a++)
	  for(int b=0; b<2; b++)
	    for(int c=0; c<2; c++) {
	      BoxKey chdkey = this->chdkey(srckey, Index3(a,b,c));
	      BoxDat& chddat = _boxvec.access(chdkey);
	      if(chddat.tag() & WAVE3D_PTS) {
		iC( zgemv(1.0, ue2uc(a,b,c), chddat.upeqnden(), 1.0, upchkval) );
	      }
	    }
      }
      //uc2ue
      CpxNumMat& v  = uc2ue(0);
      CpxNumMat& is = uc2ue(1); //LEXING: it is stored as a matrix
      CpxNumMat& up = uc2ue(2);
      CpxNumVec mid(up.m());	  setvalue(mid,cpx(0,0));
      iC( zgemv(1.0, up, upchkval, 0.0, mid) );
      for(int k=0; k<mid.m(); k++)		mid(k) = mid(k) * is(k,0);
      upeqnden.resize(v.m());		setvalue(upeqnden,cpx(0,0));
      iC( zgemv(1.0, v, mid, 0.0, upeqnden) );
      //-------------------------
      //EXTRA WORK, change role now
      BoxDat& trgdat = srcdat;
      for(int k=0; k<trgdat.undeidxvec().size(); k++)
	reqboxset.insert( trgdat.undeidxvec()[k] );
      for(int k=0; k<trgdat.vndeidxvec().size(); k++)
	reqboxset.insert( trgdat.vndeidxvec()[k] );
      for(int k=0; k<trgdat.wndeidxvec().size(); k++)
	reqboxset.insert( trgdat.wndeidxvec()[k] );
      for(int k=0; k<trgdat.xndeidxvec().size(); k++)
	reqboxset.insert( trgdat.xndeidxvec()[k] );
    }
  }
  return 0;
}

//---------------------------------------------------------------------
int Wave3d::eval_dnward_low(double W, vector<BoxKey>& trgvec)
{
  DblNumMat dep;
  DblNumMat dcp;
  NumVec<CpxNumMat> dc2de;
  NumTns<CpxNumMat> de2dc;
  NumTns<CpxNumTns> ue2dc;
  DblNumMat uep;
  iC( _mlibptr->dnward_lowfetch(W, dep, dcp, dc2de, de2dc, ue2dc, uep) );
  //------------------
  int sdof = 1;  int tdof = 1;
  int _P = P();
  for(int k=0; k<trgvec.size(); k++) {
    BoxKey trgkey = trgvec[k];
    BoxDat& trgdat = _boxvec.access(trgkey);
    if(trgdat.tag() & WAVE3D_PTS) {
      //-----------------------------------------------------------------------------
      Point3 trgctr = center(trgkey);
      //array
      CpxNumVec& dnchkval = trgdat.dnchkval();
      if(dnchkval.m()==0) {
	dnchkval.resize(dcp.n());		setvalue(dnchkval,cpx(0,0));
      }
      if(trgdat.extval().m()==0) {
	trgdat.extval().resize( trgdat.extpos().n() );		setvalue(trgdat.extval(), cpx(0,0));
      }
      DblNumMat dnchkpos(dcp.m(), dcp.n());
      for(int k=0; k<dcp.n(); k++)
	for(int d=0; d<dim(); d++)
	  dnchkpos(d,k) = dcp(d,k) + trgctr(d);
      //lists
      //-------------
      //u
      for(vector<BoxKey>::iterator vi=trgdat.undeidxvec().begin(); vi!=trgdat.undeidxvec().end(); vi++) {
	BoxKey neikey = (*vi);
	BoxDat& neidat = _boxvec.access(neikey);
	//mul
	CpxNumMat mat;	  iC( _knl.kernel(trgdat.extpos(), neidat.extpos(), neidat.extpos(), mat) );
	iC( zgemv(1.0, mat, neidat.extden(), 1.0, trgdat.extval()) );
      }
      //-------------
      //v
      double step = W/(_P-1);
      setvalue(_valfft,cpx(0,0));
      //LEXING: SPECIAL
      for(vector<BoxKey>::iterator vi=trgdat.vndeidxvec().begin(); vi!=trgdat.vndeidxvec().end(); vi++) {
	BoxKey neikey = (*vi);
	BoxDat& neidat = _boxvec.access(neikey);
	//mul
	Point3 neictr = center(neikey);		//double DD = neinde.width();
	Index3 idx;
	for(int d=0; d<dim(); d++)			 idx(d) = int(round( (trgctr[d]-neictr[d])/W )); //LEXING:CHECK
	//creat if it is missing
	if(neidat.fftcnt()==0) {
	  setvalue(_denfft, cpx(0,0));
	  CpxNumVec& neiden = neidat.upeqnden();
	  for(int k=0; k<uep.n(); k++) {
	    int a = int( round((uep(0,k)+W/2)/step) ) + _P;
	    int b = int( round((uep(1,k)+W/2)/step) ) + _P;
	    int c = int( round((uep(2,k)+W/2)/step) ) + _P;
	    _denfft(a,b,c) = neiden(k);
	  }
	  fftw_execute(_fplan);
	  neidat.upeqnden_fft() = _denfft; //COPY to the right place
	}
	CpxNumTns& neidenfft = neidat.upeqnden_fft();
	//TODO: LEXING GET THE INTERACTION TENSOR
	CpxNumTns& inttns = ue2dc(idx[0]+3,idx[1]+3,idx[2]+3);
	for(int a=0; a<2*_P; a++)
	  for(int b=0; b<2*_P; b++)
	    for(int c=0; c<2*_P; c++)
	      _valfft(a,b,c) += (neidenfft(a,b,c)*inttns(a,b,c));
	//clean if necessary
	neidat.fftcnt()++;
	if(neidat.fftcnt()==neidat.fftnum()) {
	  neidat.upeqnden_fft().resize(0,0,0);
	  neidat.fftcnt() = 0;//reset, LEXING
	}
      }
      fftw_execute(_bplan);
      //add back
      double coef = 1.0/(2*_P * 2*_P * 2*_P);
      for(int k=0; k<dcp.n(); k++) {
	int a = int( round((dcp(0,k)+W/2)/step) ) + _P;
	int b = int( round((dcp(1,k)+W/2)/step) ) + _P;
	int c = int( round((dcp(2,k)+W/2)/step) ) + _P;
	dnchkval(k) += (_valfft(a,b,c)*coef); //LEXING: VERY IMPORTANT
      }
      //-------------
      //w
      for(vector<BoxKey>::iterator vi=trgdat.wndeidxvec().begin(); vi!=trgdat.wndeidxvec().end(); vi++) {
	BoxKey neikey = (*vi);
	BoxDat& neidat = _boxvec.access(neikey);
	Point3 neictr = center(neikey);
	//upchkpos
	if(isterminal(neidat)==true && neidat.extpos().n()<uep.n()) {
	  CpxNumMat mat;		iC( _knl.kernel(trgdat.extpos(), neidat.extpos(), neidat.extpos(), mat) );
	  iC( zgemv(1.0, mat, neidat.extden(), 1.0, trgdat.extval()) );
	} else {
	  double coef = width(neikey)/W; //LEXING: SUPER IMPORTANT
	  DblNumMat upeqnpos(uep.m(), uep.n()); //local version
	  for(int k=0; k<uep.n(); k++)
	    for(int d=0; d<dim(); d++)
	      upeqnpos(d,k) = coef*uep(d,k) + neictr(d);
	  //mul
	  CpxNumMat mat;	  iC( _knl.kernel(trgdat.extpos(), upeqnpos, upeqnpos, mat) );
	  iC( zgemv(1.0, mat, neidat.upeqnden(), 1.0, trgdat.extval()) );
	}
      }
      //-------------
      //x
      for(vector<BoxKey>::iterator vi=trgdat.xndeidxvec().begin(); vi!=trgdat.xndeidxvec().end(); vi++) {
	BoxKey neikey = (*vi);
	BoxDat& neidat = _boxvec.access(neikey);
	Point3 neictr = center(neikey);
	if(isterminal(trgdat)==true && trgdat.extpos().n()<dcp.n()) {
	  CpxNumMat mat;		iC( _knl.kernel(trgdat.extpos(), neidat.extpos(), neidat.extpos(), mat) );
	  iC( zgemv(1.0, mat, neidat.extden(), 1.0, trgdat.extval()) );
	} else {
	  //mul
	  CpxNumMat mat;		  iC( _knl.kernel(dnchkpos, neidat.extpos(), neidat.extpos(), mat) );
	  iC( zgemv(1.0, mat, neidat.extden(), 1.0, dnchkval) );
	}
      }
      //-------------
      //dnchkval to dneqnden
      CpxNumMat& v  = dc2de(0);
      CpxNumMat& is = dc2de(1);
      CpxNumMat& up = dc2de(2);
      CpxNumVec mid(up.m());	  setvalue(mid,cpx(0,0));
      iC( zgemv(1.0, up, dnchkval, 0.0, mid) );
      dnchkval.resize(0); //LEXING: SAVE SPACE
      for(int k=0; k<mid.m(); k++)		mid(k) = mid(k) * is(k,0);
      CpxNumVec dneqnden(v.m());	  setvalue(dneqnden,cpx(0,0));	  iC( zgemv(1.0, v, mid, 0.0, dneqnden) );
      //-------------
      //to children or to exact points
      if(isterminal(trgdat)==true) {
	DblNumMat dneqnpos(dep.m(), dep.n());
	for(int k=0; k<dep.n(); k++)
	  for(int d=0; d<dim(); d++)
	    dneqnpos(d,k) = dep(d,k) + trgctr(d);
	//mul
	CpxNumMat mat;		  iC( _knl.kernel(trgdat.extpos(), dneqnpos, dneqnpos, mat) );
	iC( zgemv(1.0, mat, dneqnden, 1.0, trgdat.extval()) );
      } else {
	//put stuff to children
	for(int a=0; a<2; a++)
	  for(int b=0; b<2; b++)
	    for(int c=0; c<2; c++) {
	      BoxKey chdkey = this->chdkey(trgkey, Index3(a,b,c));
	      BoxDat& chddat = _boxvec.access(chdkey);
	      if(chddat.tag() & WAVE3D_PTS) {
		//mul
		if(chddat.dnchkval().m()==0) {
		  chddat.dnchkval().resize(de2dc(a,b,c).m());				  setvalue(chddat.dnchkval(), cpx(0,0));
		}
		iC( zgemv(1.0, de2dc(a,b,c), dneqnden, 1.0, chddat.dnchkval()) );
	      }
	    }
      }
    }
  }
  return 0;
}

//---------------------------------------------------------------------
int Wave3d::eval_upward_hgh_recursive(double W, Index3 nowdir, map< Index3, pair< vector<BoxKey>, vector<BoxKey> > >& hdmap, set<BndKey>& reqbndset)
{
  map< Index3, pair< vector<BoxKey>, vector<BoxKey> > >::iterator mi = hdmap.find(nowdir);
  if(mi!=hdmap.end()) {
    iC( eval_upward_hgh(W, nowdir, (*mi).second, reqbndset) );
    vector<Index3> dirvec = chddir(nowdir);
    for(int k=0; k<dirvec.size(); k++) {
      iC( eval_upward_hgh_recursive(2*W, dirvec[k], hdmap, reqbndset) );
    }
  }
  return 0;
}

//---------------------------------------------------------------------
int Wave3d::eval_dnward_hgh_recursive(double W, Index3 nowdir, map< Index3, pair< vector<BoxKey>, vector<BoxKey> > >& hdmap)
{
  map< Index3, pair< vector<BoxKey>, vector<BoxKey> > >::iterator mi = hdmap.find(nowdir);
  if(mi!=hdmap.end()) {
    vector<Index3> dirvec = chddir(nowdir);
    for(int k=0; k<dirvec.size(); k++) {
      iC( eval_dnward_hgh_recursive(2*W, dirvec[k], hdmap) );
    }
    iC( eval_dnward_hgh(W, nowdir, (*mi).second) );
  }
  return 0;
}

//---------------------------------------------------------------------
int Wave3d::eval_upward_hgh(double W, Index3 dir, pair< vector<BoxKey>, vector<BoxKey> >& hdvecs, set<BndKey>& reqbndset)
{
  double eps = 1e-12;
  DblNumMat uep;
  DblNumMat ucp;
  NumVec<CpxNumMat> uc2ue;
  NumTns<CpxNumMat> ue2uc;
  iC( _mlibptr->upward_hghfetch(W, dir, uep, ucp, uc2ue, ue2uc) );
  //---------------
  vector<BoxKey>& srcvec = hdvecs.first;
  int sdof = 1;  int tdof = 1;
  for(int k=0; k<srcvec.size(); k++) {
	BoxKey srckey = srcvec[k];
	BoxDat& srcdat = _boxvec.access(srckey);
	if(srcdat.tag() & WAVE3D_PTS) {
	  //-----------------------------------------------------------------------------
	  Point3 srcctr = center(srckey);
	  BndKey bndkey;	  bndkey.first = srckey;	  bndkey.second = dir;
	  BndDat& bnddat = _bndvec.access( bndkey );
	  CpxNumVec& upeqnden = bnddat.dirupeqnden();
	  //eval
	  CpxNumVec upchkval(ue2uc(0,0,0).m());	  setvalue(upchkval,cpx(0,0));
	  if(abs(W-1)<eps) {
		for(int a=0; a<2; a++)
		  for(int b=0; b<2; b++)
			for(int c=0; c<2; c++) {
			  BoxKey chdkey = this->chdkey(srckey, Index3(a,b,c));
			  BoxDat& chddat = _boxvec.access(chdkey);
			  if(chddat.tag() & WAVE3D_PTS) {
				CpxNumVec& chdued = chddat.upeqnden();
				iC( zgemv(1.0, ue2uc(a,b,c), chdued, 1.0, upchkval) );
			  }
			}
	  } else {
		Index3 pdir = predir(dir); //parent direction
		for(int a=0; a<2; a++)
		  for(int b=0; b<2; b++)
			for(int c=0; c<2; c++) {
			  BoxKey chdkey = this->chdkey(srckey, Index3(a,b,c));
			  BoxDat& chddat = _boxvec.access(chdkey);
			  if(chddat.tag() & WAVE3D_PTS) {
				BndKey bndkey;
				bndkey.first = chdkey;				  bndkey.second = pdir;
				BndDat& bnddat = _bndvec.access(bndkey);
				CpxNumVec& chdued = bnddat.dirupeqnden();
				iC( zgemv(1.0, ue2uc(a,b,c), chdued, 1.0, upchkval) );
			  }
			}
	  }
	  //uc2ue
	  CpxNumMat& E1 = uc2ue(0);
	  CpxNumMat& E2 = uc2ue(1);
	  CpxNumMat& E3 = uc2ue(2);
	  cpx dat0[DVMAX], dat1[DVMAX], dat2[DVMAX];
	  CpxNumVec tmp0(E3.m(), false, dat0);	  iA(DVMAX>=E3.m());
	  CpxNumVec tmp1(E2.m(), false, dat1);	  iA(DVMAX>=E2.m());
	  upeqnden.resize(E1.m());	  setvalue(upeqnden,cpx(0,0));
	  iC( zgemv(1.0, E3, upchkval, 0.0, tmp0) );
	  iC( zgemv(1.0, E2, tmp0, 0.0, tmp1) );
	  iC( zgemv(1.0, E1, tmp1, 0.0, upeqnden) );
	}
  }
  //-----------------
  //EXTRA WORK, change role
  vector<BoxKey>& trgvec = hdvecs.second;
  for(int k=0; k<trgvec.size(); k++) {
	BoxKey trgkey = trgvec[k];
	BoxDat& trgdat = _boxvec.access(trgkey);
	vector<BoxKey>& tmpvec = trgdat.fndeidxvec()[dir];
	for(int i=0; i<tmpvec.size(); i++) {
	  BoxKey srckey = tmpvec[i];
	  BndKey bndkey;		bndkey.first = srckey;		bndkey.second = dir;
	  reqbndset.insert(bndkey);
	}
  }
  return 0;
}

//---------------------------------------------------------------------
int Wave3d::eval_dnward_hgh(double W, Index3 dir, pair< vector<BoxKey>, vector<BoxKey> >& hdvecs)
{
  double eps = 1e-12;
  DblNumMat dep;
  DblNumMat dcp;
  NumVec<CpxNumMat> dc2de;
  NumTns<CpxNumMat> de2dc;
  DblNumMat uep;
  iC( _mlibptr->dnward_hghfetch(W, dir, dep, dcp, dc2de, de2dc, uep) );
  //LEXING: IMPORTANT
  vector<BoxKey>& trgvec = hdvecs.second;
  int sdof = 1;  int tdof = 1;
  for(int k=0; k<trgvec.size(); k++) {
	BoxKey trgkey = trgvec[k];
	BoxDat& trgdat = _boxvec.access(trgkey);
	if(trgdat.tag() & WAVE3D_PTS) {
	  //-----------------------------------------------------------------------------
	  Point3 trgctr = center(trgkey);
	  //1. mix
	  //get target
	  DblNumMat tmpdcp(dcp.m(),dcp.n());
	  for(int k=0; k<tmpdcp.n(); k++)
		for(int d=0; d<3; d++)
		  tmpdcp(d,k) = dcp(d,k) + trgctr(d);
	  BndKey bndkey;	  bndkey.first = trgkey;	  bndkey.second = dir;
	  BndDat& bnddat = _bndvec.access(bndkey);
	  CpxNumVec& dcv = bnddat.dirdnchkval();
	  vector<BoxKey>& tmpvec = trgdat.fndeidxvec()[dir];
	  for(int i=0; i<tmpvec.size(); i++) {
		BoxKey srckey = tmpvec[i];
		BoxDat& srcdat = _boxvec.access(srckey);
		Point3 srcctr = center(srckey);
		//difference vector
		Point3 diff;		if(trgkey>srckey)		diff = trgctr-srcctr;	  else		diff = -(srcctr-trgctr);
		diff = diff/diff.l2(); //LEXING: see wave3d_setup.cpp
		iA( nml2dir(diff,W)==dir );		//Index3 dir = nml2dir(tmp, W);
		//get source
		DblNumMat tmpuep(uep.m(),uep.n());
		for(int k=0; k<tmpuep.n(); k++)
		  for(int d=0; d<3; d++)
			tmpuep(d,k) = uep(d,k) + srcctr(d);
		BndKey bndkey;		bndkey.first = srckey;		bndkey.second = dir;
		BndDat& bnddat = _bndvec.access(bndkey);
		CpxNumVec& ued = bnddat.dirupeqnden();
		//mateix
		CpxNumMat Mts;		iC( _knl.kernel(tmpdcp, tmpuep, tmpuep, Mts) );
		//allocate space if necessary
		if(dcv.m()==0) {
		  dcv.resize(Mts.m());		  setvalue(dcv,cpx(0,0)); //LEXING: CHECK
		}
		iC( zgemv(1.0, Mts, ued, 1.0, dcv) );
	  }
	  //2. to children
	  CpxNumVec& dnchkval = dcv;
	  //dc2de
	  CpxNumMat& E1 = dc2de(0);
	  CpxNumMat& E2 = dc2de(1);
	  CpxNumMat& E3 = dc2de(2);
	  cpx dat0[DVMAX], dat1[DVMAX], dat2[DVMAX];
	  CpxNumVec tmp0(E3.m(), false, dat0);
	  CpxNumVec tmp1(E2.m(), false, dat1);
	  CpxNumVec dneqnden(E1.m(), false, dat2);
	  iC( zgemv(1.0, E3, dnchkval, 0.0, tmp0) );
	  iC( zgemv(1.0, E2, tmp0, 0.0, tmp1) );
	  iC( zgemv(1.0, E1, tmp1, 0.0, dneqnden) );
	  dnchkval.resize(0); //LEXING: SAVE SPACE
	  //eval
	  if(abs(W-1)<eps) {
		for(int a=0; a<2; a++)
		  for(int b=0; b<2; b++)
			for(int c=0; c<2; c++) {
			  BoxKey chdkey = this->chdkey(trgkey, Index3(a,b,c));
			  BoxDat& chddat = _boxvec.access(chdkey);
			  if(chddat.tag() & WAVE3D_PTS) {
				CpxNumVec& chddcv = chddat.dnchkval();
				if(chddcv.m()==0) {
				  chddcv.resize(de2dc(a,b,c).m());				  setvalue(chddcv,cpx(0,0));
				}
				iC( zgemv(1.0, de2dc(a,b,c), dneqnden, 1.0, chddcv) );
			  }
			}
	  } else {
		Index3 pdir = predir(dir); //LEXING: CHECK
		for(int a=0; a<2; a++)
		  for(int b=0; b<2; b++)
			for(int c=0; c<2; c++) {
			  BoxKey chdkey = this->chdkey(trgkey, Index3(a,b,c));
			  BoxDat& chddat = _boxvec.access(chdkey);
			  if(chddat.tag() & WAVE3D_PTS) {
				BndKey bndkey;				  bndkey.first = chdkey;				  bndkey.second = pdir;
				BndDat& bnddat = _bndvec.access(bndkey);
				CpxNumVec& chddcv = bnddat.dirdnchkval();
				if(chddcv.m()==0) {
				  chddcv.resize(de2dc(a,b,c).m());				  setvalue(chddcv,cpx(0,0));
				}
				iC( zgemv(1.0, de2dc(a,b,c), dneqnden, 1.0, chddcv) );
			  }
			}
	  }
	}
  }
  //-----------------
  //EXTRA WORK, change role
  vector<BoxKey>& srcvec = hdvecs.first;
  for(int k=0; k<srcvec.size(); k++) {
	BoxKey srckey = srcvec[k];
	BoxDat& srcdat = _boxvec.access(srckey);
	if(srcdat.tag() & WAVE3D_PTS) {
	  BndKey bndkey;	  bndkey.first = srckey;	  bndkey.second = dir;
	  BndDat& bnddat = _bndvec.access( bndkey );
	  bnddat.dirupeqnden().resize(0);
	}
  }
  for(int k=0; k<trgvec.size(); k++) {
	BoxKey trgkey = trgvec[k];
	BoxDat& trgdat = _boxvec.access(trgkey);
	if(trgdat.tag() & WAVE3D_PTS) {
	  vector<BoxKey>& tmpvec = trgdat.fndeidxvec()[dir];
	  for(int i=0; i<tmpvec.size(); i++) {
		BoxKey srckey = tmpvec[i];
		BoxDat& srcdat = _boxvec.access(srckey);
		BndKey bndkey;		bndkey.first = srckey;		bndkey.second = dir;
		BndDat& bnddat = _bndvec.access(bndkey);
		bnddat.dirupeqnden().resize(0);
	  }
	}
  }
  return 0;
}

