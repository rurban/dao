/*=========================================================================================
  This file is a part of a virtual machine for the Dao programming language.
  Copyright (C) 2006-2011, Fu Limin. Email: fu@daovm.net, limin.fu@yahoo.com

  This software is free software; you can redistribute it and/or modify it under the terms 
  of the GNU Lesser General Public License as published by the Free Software Foundation; 
  either version 2.1 of the License, or (at your option) any later version.

  This software is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
  See the GNU Lesser General Public License for more details.
  =========================================================================================*/

#include"daoGC.h"
#include"daoMap.h"
#include"daoClass.h"
#include"daoObject.h"
#include"daoNumtype.h"
#include"daoContext.h"
#include"daoProcess.h"
#include"daoRoutine.h"
#include"daoNamespace.h"
#include"daoThread.h"

void GC_Lock();
void GC_Unlock();

DArray *dao_callback_data = NULL;

DaoCallbackData* DaoCallbackData_New( DaoMethod *callback, DaoValue *userdata )
{
	DaoCallbackData *self;
	if( callback == NULL ) return NULL;
	if( callback->type < DAO_FUNCTREE || callback->type > DAO_FUNCTION ) return NULL;
	self = (DaoCallbackData*) calloc( 1, sizeof(DaoCallbackData) );
	self->callback = callback;
	DaoValue_Copy( userdata, & self->userdata );
	GC_Lock();
	DArray_Append( dao_callback_data, self );
	GC_Unlock();
	return self;
}
static void DaoCallbackData_Delete( DaoCallbackData *self )
{
	GC_DecRC( self->userdata );
	dao_free( self );
}
static void DaoCallbackData_DeleteByCallback( DaoValue *callback )
{
	DaoCallbackData *cd = NULL;
	int i;
	if( dao_callback_data->size ==0 ) return;
	GC_Lock();
	for(i=0; i<dao_callback_data->size; i++){
		cd = (DaoCallbackData*) dao_callback_data->items.pValue[i];
		if( cd->callback == (DaoMethod*) callback ){
			DaoCallbackData_Delete( cd );
			DArray_Erase( dao_callback_data, i, 1 );
			i--;
		}
	}
	GC_Unlock();
}
static void DaoCallbackData_DeleteByUserdata( DaoValue *userdata )
{
	DaoCallbackData *cd = NULL;
	int i;
	if( userdata == NULL ) return;
	if( dao_callback_data->size ==0 ) return;
	GC_Lock();
	for(i=0; i<dao_callback_data->size; i++){
		cd = (DaoCallbackData*) dao_callback_data->items.pValue[i];
		if( cd->userdata == userdata ){
			DaoCallbackData_Delete( cd );
			DArray_Erase( dao_callback_data, i, 1 );
			i--;
		}
	}
	GC_Unlock();
}


#define GC_IN_POOL 1
#define GC_MARKED  2

#if DEBUG
#if 0
#define DEBUG_TRACE
#endif
#endif

#ifdef DEBUG_TRACE
#include <execinfo.h>
void print_trace()
{
	void  *array[100];
	size_t i, size = backtrace (array, 100);
	char **strings = backtrace_symbols (array, size);
	FILE *debug = fopen( "debug.txt", "w+" );
	fprintf (debug, "===========================================\n");
	fprintf (debug, "Obtained %zd stack frames.\n", size);
	printf ("===========================================\n");
	printf ("Obtained %zd stack frames.\n", size);
	for (i = 0; i < size; i++){
		printf ("%s\n", strings[i]);
		fprintf (debug,"%s\n", strings[i]);
	}
	/* comment this to cause leaking, so that valgrind will print the trace with line numbers */
	free (strings);
	fflush( debug );
	fclose( debug );
	fflush( stdout );
}
#endif

#define gcDecreaseRefCountThenBreak( p ) { if( p ){ (p)->refCount --; (p) = NULL; } }

static void DaoGC_DecRC2( DaoValue *p, int change );
static void cycRefCountDecrement( DaoValue *value );
static void cycRefCountIncrement( DaoValue *value );
static void cycRefCountDecrements( DArray * values );
static void cycRefCountIncrements( DArray * values );
static void directRefCountDecrement( DArray * values );
static void cycRefCountDecrementsT( DTuple * values );
static void cycRefCountIncrementsT( DTuple * values );
static void directRefCountDecrementT( DTuple * values );
static void cycRefCountDecrementMapValue( DMap *dmap );
static void cycRefCountIncrementMapValue( DMap *dmap );
static void directRefCountDecrementMapValue( DMap *dmap );
static void cycRefCountDecreScan();
static void cycRefCountIncreScan();
static void freeGarbage();
static void InitGC();

#ifdef DAO_WITH_THREAD
static void markAliveObjects( DaoValue *root );
static void recycleGarbage( void * );
static void tryInvoke();
#endif

static void DaoLateDeleter_Init();
static void DaoLateDeleter_Finish();
static void DaoLateDeleter_Update();

typedef struct DaoGarbageCollector  DaoGarbageCollector;
struct DaoGarbageCollector
{
	DArray   *pool[2];
	DArray   *objAlive;

	short     work, idle;
	int       gcMin, gcMax;
	int       count, boundary;
	int       ii, jj;
	short     busy;
	short     locked;
	short     workType;
	short     finalizing;

#ifdef DAO_WITH_THREAD
	DThread   thread;

	DMutex    mutex_switch_heap;
	DMutex    mutex_start_gc;
	DMutex    mutex_block_mutator;

	DCondVar  condv_start_gc;
	DCondVar  condv_block_mutator;
#endif
};
static DaoGarbageCollector gcWorker = { { NULL, NULL }, NULL };

extern int ObjectProfile[100];
extern int daoCountMBS;
extern int daoCountArray;

int DaoGC_Min( int n /*=-1*/ )
{
	int prev = gcWorker.gcMin;
	if( n > 0 ) gcWorker.gcMin = n;
	return prev;
}
int DaoGC_Max( int n /*=-1*/ )
{
	int prev = gcWorker.gcMax;
	if( n > 0 ) gcWorker.gcMax = n;
	return prev;
}

void InitGC()
{
	if( gcWorker.objAlive != NULL ) return;
	DaoLateDeleter_Init();

	gcWorker.pool[0] = DArray_New(0);
	gcWorker.pool[1] = DArray_New(0);
	gcWorker.objAlive = DArray_New(0);

	gcWorker.idle = 0;
	gcWorker.work = 1;
	gcWorker.finalizing = 0;

	gcWorker.gcMin = 1000;
	gcWorker.gcMax = 100 * gcWorker.gcMin;
	gcWorker.count = 0;
	gcWorker.workType = 0;
	gcWorker.ii = 0;
	gcWorker.jj = 0;
	gcWorker.busy = 0;
	gcWorker.locked = 0;

#ifdef DAO_WITH_THREAD
	DThread_Init( & gcWorker.thread );
	DMutex_Init( & gcWorker.mutex_switch_heap );
	DMutex_Init( & gcWorker.mutex_start_gc );
	DMutex_Init( & gcWorker.mutex_block_mutator );
	DCondVar_Init( & gcWorker.condv_start_gc );
	DCondVar_Init( & gcWorker.condv_block_mutator );
#endif
}
void DaoStartGC()
{
	InitGC();
#ifdef DAO_WITH_THREAD
	DThread_Start( & gcWorker.thread, recycleGarbage, NULL );
#endif
}
static void DaoGC_DecRC2( DaoValue *p, int change )
{
	DaoType *tp, *tp2;
	DNode *node;
	const short idle = gcWorker.idle;
	int i, n;
	p->refCount += change;

#if 0
	if( p->refCount == 0 ){
		/* free some memory, but do not delete it here,
		 * because it may be a member of DValue,
		 * and the DValue.t is not yet set to zero. */
		switch( p->type ){
		case DAO_LIST :
			{
				DaoList *list = (DaoList*) p;
				n = list->items->size;
				if( list->unitype && list->unitype->nested->size ){
					tp = list->unitype->nested->items.pType[0];
					for(i=0; i<n; i++){
						DValue *it = list->items->data + i;
						if( it->t > DAO_DOUBLE && it->t < DAO_ENUM ) DValue_Clear( it );
					}
				}
				break;
			}
		case DAO_TUPLE :
			{
				DaoTuple *tuple = (DaoTuple*) p;
				n = tuple->items->size;
				for(i=0; i<n; i++){
					DValue *it = tuple->items->data + i;
					if( it->t > DAO_DOUBLE && it->t < DAO_ENUM ) DValue_Clear( it );
				}
				break;
			}
		case DAO_MAP :
			{
				DaoMap *map = (DaoMap*) p;
				tp2 = map->unitype;
				if( tp2 == NULL || tp2->nested->size != 2 ) break;
				tp = tp2->nested->items.pType[0];
				tp2 = tp2->nested->items.pType[1];
				if( tp->tid >DAO_DOUBLE && tp2->tid >DAO_DOUBLE && tp->tid <DAO_ENUM && tp2->tid <DAO_ENUM ){
					/* DaoMap_Clear( map ); this is NOT thread safe, there is RC update scan */
					i = 0;
					node = DMap_First( map->items );
					for( ; node != NULL; node = DMap_Next( map->items, node ) ) {
						DValue_Clear( node->key.pValue );
						DValue_Clear( node->value.pValue );
						node->key.pValue->t = DAO_INTEGER;
						node->key.pValue->v.i = ++ i; /* keep key ordering */
					}
				}else if( tp2->tid >DAO_DOUBLE && tp2->tid < DAO_ENUM ){
					node = DMap_First( map->items );
					for( ; node != NULL; node = DMap_Next( map->items, node ) ) {
						DValue_Clear( node->value.pValue );
					}
				}else if( tp->tid >DAO_DOUBLE && tp->tid < DAO_ENUM ){
					i = 0;
					node = DMap_First( map->items );
					for( ; node != NULL; node = DMap_Next( map->items, node ) ) {
						DValue_Clear( node->key.pValue );
						node->key.pValue->t = DAO_INTEGER;
						node->key.pValue->v.i = ++ i; /* keep key ordering */
					}
				}
				break;
			}
		case DAO_ARRAY :
			{
#ifdef DAO_WITH_NUMARRAY
				DaoArray *array = (DaoArray*) p;
				DaoArray_ResizeVector( array, 0 );
#endif
				break;
			}
#if 0
		case DAO_OBJECT :
			{
				DaoObject *object = (DaoObject*) p;
				if( object->objData == NULL ) break;
				n = object->objData->size;
				m = 0;
				for(i=0; i<n; i++){
					DValue *it = object->objValues + i;
					if( it->t && it->t < DAO_ARRAY ){
						DValue_Clear( it );
						m ++;
					}
				}
				if( m == n ) DVaTuple_Clear( object->objData );
				break;
			}
#endif
		case DAO_CONTEXT :
			{
				DaoContext *ctx = (DaoContext*) p;
				if( ctx->regValues ) dao_free( ctx->regValues );
				ctx->regValues = NULL;
				n = ctx->regArray->size;
				for(i=0; i<n; i++){
					DValue *it = ctx->regArray->data + i;
					if( it->t > DAO_DOUBLE && it->t < DAO_ENUM ) DValue_Clear( it );
				}
				break;
			}
		default : break;
		}
	}
#endif
	/* already in idle pool */
	if( p->gcState[ idle ] & GC_IN_POOL ) return;
	p->gcState[ idle ] = GC_IN_POOL;
	DArray_Append( gcWorker.pool[ idle ], p );
#if 0
	if( p->refCount ==0 ) return; /* already counted */
#endif
	switch( p->type ){
	case DAO_OBJECT :
		if( p->xObject.objData ) gcWorker.count += p->xObject.objData->size + 1;
		break;
	case DAO_LIST : gcWorker.count += p->xList.items->size + 1; break;
	case DAO_MAP  : gcWorker.count += p->xMap.items->size + 1; break;
#ifdef DAO_WITH_NUMARRAY
	case DAO_ARRAY : gcWorker.count += p->xArray.size + 1; break;
#endif
	case DAO_TUPLE : gcWorker.count += p->xTuple.items->size; break;
	case DAO_CONTEXT : gcWorker.count += p->xContext.regArray->size; break;
	default: gcWorker.count += 1; break;
	}
}
#ifdef DAO_WITH_THREAD

void GC_Lock()
{
	DMutex_Lock( & gcWorker.mutex_switch_heap );
}
void GC_Unlock()
{
	DMutex_Unlock( & gcWorker.mutex_switch_heap );
}

/* Concurrent Garbage Collector */

void DaoGC_DecRC( DaoValue *p )
{
	const short idle = gcWorker.idle;
	if( ! p ) return;

#ifdef DEBUG_TRACE
	if( p == 0x2531c40 ){
		print_trace();
		printf( "rc: %i\n", p->refCount );
	}
#endif
	DMutex_Lock( & gcWorker.mutex_switch_heap );

	DaoGC_DecRC2( p, -1 );

	DMutex_Unlock( & gcWorker.mutex_switch_heap );

	if( gcWorker.pool[ idle ]->size > gcWorker.gcMin ) tryInvoke();
}
void DaoFinishGC()
{
	gcWorker.gcMin = 0;
	gcWorker.finalizing = 1;
	tryInvoke();
	DThread_Join( & gcWorker.thread );

	DArray_Delete( gcWorker.pool[0] );
	DArray_Delete( gcWorker.pool[1] );
	DArray_Delete( gcWorker.objAlive );
	DaoLateDeleter_Finish();
	gcWorker.objAlive = NULL;
}
void DaoGC_IncRC( DaoValue *p )
{
	if( ! p ) return;
#ifdef DEBUG_TRACE
	if( p == 0x1011a0 ) print_trace();
#endif
	if( p->refCount == 0 ){
		p->refCount ++;
		return;
	}

	DMutex_Lock( & gcWorker.mutex_switch_heap );
	p->refCount ++;
	p->cycRefCount ++;
	DMutex_Unlock( & gcWorker.mutex_switch_heap );
}
void DaoGC_ShiftRC( DaoValue *up, DaoValue *down )
{
	const short idle = gcWorker.idle;
	if( up == down ) return;

	DMutex_Lock( & gcWorker.mutex_switch_heap );

	if( up ){
		up->refCount ++;
		up->cycRefCount ++;
	}
	if( down ) DaoGC_DecRC2( down, -1 );

	DMutex_Unlock( & gcWorker.mutex_switch_heap );

	if( down && gcWorker.pool[ idle ]->size > gcWorker.gcMin ) tryInvoke();
}

void DaoGC_IncRCs( DArray *list )
{
	size_t i;
	DaoValue **values;

	if( list->size == 0 ) return;
	values = list->items.pValue;
	DMutex_Lock( & gcWorker.mutex_switch_heap );
	for( i=0; i<list->size; i++){
		if( values[i] ){
			values[i]->refCount ++;
			values[i]->cycRefCount ++;
		}
	}
	DMutex_Unlock( & gcWorker.mutex_switch_heap );
}
void DaoGC_DecRCs( DArray *list )
{
	size_t i;
	DaoValue **values;
	const short idle = gcWorker.idle;
	if( list==NULL || list->size == 0 ) return;
	values = list->items.pValue;
	DMutex_Lock( & gcWorker.mutex_switch_heap );
	for( i=0; i<list->size; i++) if( values[i] ) DaoGC_DecRC2( values[i], -1 );
	DMutex_Unlock( & gcWorker.mutex_switch_heap );
	if( gcWorker.pool[ idle ]->size > gcWorker.gcMin ) tryInvoke();
}

void tryInvoke()
{
	DMutex_Lock( & gcWorker.mutex_start_gc );
	if( gcWorker.count >= gcWorker.gcMin ) DCondVar_Signal( & gcWorker.condv_start_gc );
	DMutex_Unlock( & gcWorker.mutex_start_gc );

	DMutex_Lock( & gcWorker.mutex_block_mutator );
	if( gcWorker.count >= gcWorker.gcMax ){
		DThreadData *thdData = DThread_GetSpecific();
		if( thdData && ! ( thdData->state & DTHREAD_NO_PAUSE ) )
			DCondVar_TimedWait( & gcWorker.condv_block_mutator, & gcWorker.mutex_block_mutator, 0.001 );
	}
	DMutex_Unlock( & gcWorker.mutex_block_mutator );
}

void recycleGarbage( void *p )
{
	while(1){
		if( gcWorker.finalizing && (gcWorker.pool[0]->size + gcWorker.pool[1]->size) ==0 ) break;
		if( ! gcWorker.finalizing ){
			DMutex_Lock( & gcWorker.mutex_block_mutator );
			DCondVar_BroadCast( & gcWorker.condv_block_mutator );
			DMutex_Unlock( & gcWorker.mutex_block_mutator );

			DMutex_Lock( & gcWorker.mutex_start_gc );
			DCondVar_TimedWait( & gcWorker.condv_start_gc, & gcWorker.mutex_start_gc, 0.1 );
			DMutex_Unlock( & gcWorker.mutex_start_gc );
			if( gcWorker.count < gcWorker.gcMin ) continue;
		}
		DaoLateDeleter_Update();

		DMutex_Lock( & gcWorker.mutex_switch_heap );
		gcWorker.count = 0;
		gcWorker.work = gcWorker.idle;
		gcWorker.idle = ! gcWorker.work;
		DMutex_Unlock( & gcWorker.mutex_switch_heap );

		cycRefCountDecreScan();
		cycRefCountIncreScan();
		freeGarbage();

	}
	DThread_Exit( & gcWorker.thread );
}

void cycRefCountDecreScan()
{
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	DNode *node;
	size_t i, k;
	for( i=0; i<pool->size; i++ )
		pool->items.pValue[i]->cycRefCount = pool->items.pValue[i]->refCount;

	for( i=0; i<pool->size; i++ ){
		DaoValue *value = pool->items.pValue[i];
		switch( value->type ){
#ifdef DAO_WITH_NUMARRAY
		case DAO_ARRAY :
			{
				DaoArray *array = (DaoArray*) value;
				cycRefCountDecrement( (DaoValue*) array->unitype );
				cycRefCountDecrement( (DaoValue*) array->meta );
				break;
			}
#endif
		case DAO_TUPLE :
			{
				DaoTuple *tuple = (DaoTuple*) value;
				cycRefCountDecrement( (DaoValue*) tuple->unitype );
				cycRefCountDecrement( (DaoValue*) tuple->meta );
				cycRefCountDecrementsT( tuple->items );
				break;
			}
		case DAO_LIST :
			{
				DaoList *list = (DaoList*) value;
				cycRefCountDecrement( (DaoValue*) list->unitype );
				cycRefCountDecrement( (DaoValue*) list->meta );
				cycRefCountDecrements( list->items );
				break;
			}
		case DAO_MAP :
			{
				DaoMap *map = (DaoMap*) value;
				cycRefCountDecrement( (DaoValue*) map->unitype );
				cycRefCountDecrement( (DaoValue*) map->meta );
				node = DMap_First( map->items );
				for( ; node != NULL; node = DMap_Next( map->items, node ) ) {
					cycRefCountDecrement( node->key.pValue );
					cycRefCountDecrement( node->value.pValue );
				}
				break;
			}
		case DAO_OBJECT :
			{
				DaoObject *obj = (DaoObject*) value;
				cycRefCountDecrementsT( obj->superObject );
				cycRefCountDecrementsT( obj->objData );
				cycRefCountDecrement( (DaoValue*) obj->meta );
				cycRefCountDecrement( (DaoValue*) obj->myClass );
				break;
			}
		case DAO_CDATA : case DAO_CTYPE :
			{
				DaoCData *cdata = (DaoCData*) value;
				cycRefCountDecrement( (DaoValue*) cdata->meta );
				cycRefCountDecrement( (DaoValue*) cdata->daoObject );
				cycRefCountDecrement( (DaoValue*) cdata->ctype );
				break;
			}
		case DAO_FUNCTREE :
			{
				DaoFunctree *meta = (DaoFunctree*) value;
				cycRefCountDecrement( (DaoValue*) meta->space );
				cycRefCountDecrement( (DaoValue*) meta->host );
				cycRefCountDecrement( (DaoValue*) meta->unitype );
				cycRefCountDecrements( meta->routines );
				break;
			}
		case DAO_ROUTINE :
		case DAO_FUNCTION :
			{
				DaoRoutine *rout = (DaoRoutine*)value;
				cycRefCountDecrement( (DaoValue*) rout->routType );
				cycRefCountDecrement( (DaoValue*) rout->routHost );
				cycRefCountDecrement( (DaoValue*) rout->nameSpace );
				cycRefCountDecrements( rout->routConsts );
				if( rout->type == DAO_ROUTINE ){
					cycRefCountDecrement( (DaoValue*) rout->upRoutine );
					cycRefCountDecrement( (DaoValue*) rout->upContext );
					cycRefCountDecrement( (DaoValue*) rout->original );
					cycRefCountDecrement( (DaoValue*) rout->specialized );
					cycRefCountDecrements( rout->regType );
					cycRefCountDecrementMapValue( rout->abstypes );
				}
				break;
			}
		case DAO_CLASS :
			{
				DaoClass *klass = (DaoClass*)value;
				cycRefCountDecrementMapValue(klass->abstypes );
				cycRefCountDecrement( (DaoValue*) klass->clsType );
				cycRefCountDecrement( (DaoValue*) klass->classRoutine );
				cycRefCountDecrements( klass->cstData );
				cycRefCountDecrements( klass->glbData );
				cycRefCountDecrements( klass->objDataDefault );
				cycRefCountDecrements( klass->superClass );
				cycRefCountDecrements( klass->objDataType );
				cycRefCountDecrements( klass->glbDataType );
				cycRefCountDecrements( klass->references );
				break;
			}
		case DAO_INTERFACE :
			{
				DaoInterface *inter = (DaoInterface*)value;
				cycRefCountDecrementMapValue( inter->methods );
				cycRefCountDecrements( inter->supers );
				cycRefCountDecrement( (DaoValue*) inter->abtype );
				break;
			}
		case DAO_CONTEXT :
			{
				DaoContext *ctx = (DaoContext*)value;
				cycRefCountDecrement( (DaoValue*) ctx->object );
				cycRefCountDecrement( (DaoValue*) ctx->routine );
				cycRefCountDecrementsT( ctx->regArray );
				break;
			}
		case DAO_NAMESPACE :
			{
				DaoNameSpace *ns = (DaoNameSpace*) value;
				cycRefCountDecrements( ns->cstData );
				cycRefCountDecrements( ns->varData );
				cycRefCountDecrements( ns->varType );
				cycRefCountDecrements( ns->cmodule->cmethods );
				cycRefCountDecrements( ns->mainRoutines );
				cycRefCountDecrementMapValue( ns->abstypes );
				for(k=0; k<ns->cmodule->ctypers->size; k++){
					DaoTypeBase *typer = (DaoTypeBase*)ns->cmodule->ctypers->items.pValue[k];
					if( typer->priv == NULL ) continue;
					cycRefCountDecrementMapValue( typer->priv->values );
					cycRefCountDecrementMapValue( typer->priv->methods );
				}
				break;
			}
		case DAO_TYPE :
			{
				DaoType *abtp = (DaoType*) value;
				cycRefCountDecrement( abtp->aux );
				cycRefCountDecrement( abtp->value );
				cycRefCountDecrements( abtp->nested );
				if( abtp->interfaces ){
					node = DMap_First( abtp->interfaces );
					for( ; node != NULL; node = DMap_Next( abtp->interfaces, node ) )
						cycRefCountDecrement( node->key.pValue );
				}
				break;
			}
		case DAO_FUTURE :
			{
				DaoFuture *future = (DaoFuture*) value;
				cycRefCountDecrement( future->value );
				cycRefCountDecrement( (DaoValue*) future->unitype );
				cycRefCountDecrement( (DaoValue*) future->context );
				cycRefCountDecrement( (DaoValue*) future->process );
				cycRefCountDecrement( (DaoValue*) future->precondition );
				break;
			}
		case DAO_VMPROCESS :
			{
				DaoVmProcess *vmp = (DaoVmProcess*) value;
				DaoVmFrame *frame = vmp->firstFrame;
				cycRefCountDecrement( vmp->returned );
				cycRefCountDecrements( vmp->parResume );
				cycRefCountDecrements( vmp->parYield );
				cycRefCountDecrements( vmp->exceptions );
				while( frame ){
					cycRefCountDecrement( (DaoValue*) frame->context );
					frame = frame->next;
				}
				break;
			}
		default: break;
		}
	}
}
void cycRefCountIncreScan()
{
	const short work = gcWorker.work;
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	size_t i, j;

	for(j=0; j<2; j++){
		for( i=0; i<pool->size; i++ ){
			DaoValue *value = pool->items.pValue[i];
			if( value->gcState[work] & GC_MARKED ) continue;
			if( value->type == DAO_CDATA && (value->refCount ==0 || value->cycRefCount ==0) ){
				DaoCData *cdata = (DaoCData*) value;
				DaoCDataCore *core = (DaoCDataCore*)cdata->typer->priv;
				if( !(cdata->attribs & DAO_CDATA_FREE) ) continue;
				if( cdata->data == NULL || core == NULL ) continue;
				if( core->DelTest == NULL ) continue;
				if( core->DelTest( cdata->data ) ) continue;
				DaoCData_SetExtReference( cdata, 1 );
			}
			if( value->cycRefCount >0 ) markAliveObjects( value );
		}
	}
}
void markAliveObjects( DaoValue *root )
{
	const short work = gcWorker.work;
	DNode *node;
	size_t i, k;
	DArray *objAlive = gcWorker.objAlive;
	objAlive->size = 0;
	root->gcState[work] |= GC_MARKED;
	DArray_Append( objAlive, root );

	for( i=0; i<objAlive->size; i++){
		DaoValue *value = objAlive->items.pValue[i];
		switch( value->type ){
#ifdef DAO_WITH_NUMARRAY
		case DAO_ARRAY :
			{
				DaoArray *array = (DaoArray*) value;
				cycRefCountIncrement( (DaoValue*) array->unitype );
				cycRefCountIncrement( (DaoValue*) array->meta );
				break;
			}
#endif
		case DAO_TUPLE :
			{
				DaoTuple *tuple= (DaoTuple*) value;
				cycRefCountIncrement( (DaoValue*) tuple->unitype );
				cycRefCountIncrement( (DaoValue*) tuple->meta );
				cycRefCountIncrementsT( tuple->items );
				break;
			}
		case DAO_LIST :
			{
				DaoList *list= (DaoList*) value;
				cycRefCountIncrement( (DaoValue*) list->unitype );
				cycRefCountIncrement( (DaoValue*) list->meta );
				cycRefCountIncrements( list->items );
				break;
			}
		case DAO_MAP :
			{
				DaoMap *map = (DaoMap*)value;
				cycRefCountIncrement( (DaoValue*) map->unitype );
				cycRefCountIncrement( (DaoValue*) map->meta );
				node = DMap_First( map->items );
				for( ; node != NULL; node = DMap_Next( map->items, node ) ){
					cycRefCountIncrement( node->key.pValue );
					cycRefCountIncrement( node->value.pValue );
				}
				break;
			}
		case DAO_OBJECT :
			{
				DaoObject *obj = (DaoObject*) value;
				cycRefCountIncrementsT( obj->superObject );
				cycRefCountIncrementsT( obj->objData );
				cycRefCountIncrement( (DaoValue*) obj->meta );
				cycRefCountIncrement( (DaoValue*) obj->myClass );
				break;
			}
		case DAO_CDATA : case DAO_CTYPE :
			{
				DaoCData *cdata = (DaoCData*) value;
				cycRefCountIncrement( (DaoValue*) cdata->meta );
				cycRefCountIncrement( (DaoValue*) cdata->daoObject );
				cycRefCountIncrement( (DaoValue*) cdata->ctype );
				break;
			}
		case DAO_FUNCTREE :
			{
				DaoFunctree *meta = (DaoFunctree*) value;
				cycRefCountIncrement( (DaoValue*) meta->space );
				cycRefCountIncrement( (DaoValue*) meta->host );
				cycRefCountIncrement( (DaoValue*) meta->unitype );
				cycRefCountIncrements( meta->routines );
				break;
			}
		case DAO_ROUTINE :
		case DAO_FUNCTION :
			{
				DaoRoutine *rout = (DaoRoutine*) value;
				cycRefCountIncrement( (DaoValue*) rout->routType );
				cycRefCountIncrement( (DaoValue*) rout->routHost );
				cycRefCountIncrement( (DaoValue*) rout->nameSpace );
				cycRefCountIncrements( rout->routConsts );
				if( rout->type == DAO_ROUTINE ){
					cycRefCountIncrement( (DaoValue*) rout->upRoutine );
					cycRefCountIncrement( (DaoValue*) rout->upContext );
					cycRefCountIncrement( (DaoValue*) rout->original );
					cycRefCountIncrement( (DaoValue*) rout->specialized );
					cycRefCountIncrements( rout->regType );
					cycRefCountIncrementMapValue( rout->abstypes );
				}
				break;
			}
		case DAO_CLASS :
			{
				DaoClass *klass = (DaoClass*) value;
				cycRefCountIncrementMapValue( klass->abstypes );
				cycRefCountIncrement( (DaoValue*) klass->clsType );
				cycRefCountIncrement( (DaoValue*) klass->classRoutine );
				cycRefCountIncrements( klass->cstData );
				cycRefCountIncrements( klass->glbData );
				cycRefCountIncrements( klass->objDataDefault );
				cycRefCountIncrements( klass->superClass );
				cycRefCountIncrements( klass->objDataType );
				cycRefCountIncrements( klass->glbDataType );
				cycRefCountDecrements( klass->references );
				break;
			}
		case DAO_INTERFACE :
			{
				DaoInterface *inter = (DaoInterface*)value;
				cycRefCountIncrementMapValue( inter->methods );
				cycRefCountIncrements( inter->supers );
				cycRefCountIncrement( (DaoValue*) inter->abtype );
				break;
			}
		case DAO_CONTEXT :
			{
				DaoContext *ctx = (DaoContext*)value;
				cycRefCountIncrement( (DaoValue*) ctx->object );
				cycRefCountIncrement( (DaoValue*) ctx->routine );
				cycRefCountIncrementsT( ctx->regArray );
				break;
			}
		case DAO_NAMESPACE :
			{
				DaoNameSpace *ns = (DaoNameSpace*) value;
				cycRefCountIncrements( ns->cstData );
				cycRefCountIncrements( ns->varData );
				cycRefCountIncrements( ns->varType );
				cycRefCountIncrements( ns->cmodule->cmethods );
				cycRefCountIncrements( ns->mainRoutines );
				cycRefCountIncrementMapValue( ns->abstypes );
				for(k=0; k<ns->cmodule->ctypers->size; k++){
					DaoTypeBase *typer = (DaoTypeBase*)ns->cmodule->ctypers->items.pValue[k];
					if( typer->priv == NULL ) continue;
					cycRefCountIncrementMapValue( typer->priv->values );
					cycRefCountIncrementMapValue( typer->priv->methods );
				}
				break;
			}
		case DAO_TYPE :
			{
				DaoType *abtp = (DaoType*) value;
				cycRefCountIncrement( abtp->aux );
				cycRefCountIncrement( abtp->value );
				cycRefCountIncrements( abtp->nested );
				if( abtp->interfaces ){
					node = DMap_First( abtp->interfaces );
					for( ; node != NULL; node = DMap_Next( abtp->interfaces, node ) )
						cycRefCountIncrement( node->key.pValue );
				}
				break;
			}
		case DAO_FUTURE :
			{
				DaoFuture *future = (DaoFuture*) value;
				cycRefCountIncrement( future->value );
				cycRefCountIncrement( (DaoValue*) future->unitype );
				cycRefCountIncrement( (DaoValue*) future->context );
				cycRefCountIncrement( (DaoValue*) future->process );
				cycRefCountIncrement( (DaoValue*) future->precondition );
				break;
			}
		case DAO_VMPROCESS :
			{
				DaoVmProcess *vmp = (DaoVmProcess*) value;
				DaoVmFrame *frame = vmp->firstFrame;
				cycRefCountIncrement( vmp->returned );
				cycRefCountIncrements( vmp->parResume );
				cycRefCountIncrements( vmp->parYield );
				cycRefCountIncrements( vmp->exceptions );
				while( frame ){
					cycRefCountIncrement( (DaoValue*) frame->context );
					frame = frame->next;
				}
				break;
			}
		default: break;
		}
	}
}

void freeGarbage()
{
	DaoTypeBase *typer;
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	DNode *node;
	size_t i, k;
	const short work = gcWorker.work;
	const short idle = gcWorker.idle;

	for( i=0; i<pool->size; i++ ){
		DaoValue *value = pool->items.pValue[i];
		value->gcState[work] = 0;

		if( value->cycRefCount == 0 || value->refCount ==0 ){

			DMutex_Lock( & gcWorker.mutex_switch_heap );
			switch( value->type ){

#ifdef DAO_WITH_NUMARRAY
			case DAO_ARRAY :
				{
					DaoArray *array = (DaoArray*) value;
					gcDecreaseRefCountThenBreak( array->unitype );
					gcDecreaseRefCountThenBreak( array->meta );
					break;
				}
#endif
			case DAO_TUPLE :
				{
					DaoTuple *tuple = (DaoTuple*) value;
					directRefCountDecrementT( tuple->items );
					gcDecreaseRefCountThenBreak( tuple->unitype );
					gcDecreaseRefCountThenBreak( tuple->meta );
					break;
				}
			case DAO_LIST :
				{
					DaoList *list = (DaoList*) value;
					directRefCountDecrement( list->items );
					gcDecreaseRefCountThenBreak( list->unitype );
					gcDecreaseRefCountThenBreak( list->meta );
					break;
				}
			case DAO_MAP :
				{
					DaoMap *map = (DaoMap*) value;
					node = DMap_First( map->items );
					for( ; node != NULL; node = DMap_Next( map->items, node ) ){
						node->key.pValue->refCount --;
						node->value.pValue->refCount --;
						node->key.pValue = NULL;
						node->value.pValue = NULL;
					}
					DMap_Clear( map->items );
					gcDecreaseRefCountThenBreak( map->unitype );
					gcDecreaseRefCountThenBreak( map->meta );
					break;
				}
			case DAO_OBJECT :
				{
					DaoObject *obj = (DaoObject*) value;
					directRefCountDecrementT( obj->superObject );
					directRefCountDecrementT( obj->objData );
					gcDecreaseRefCountThenBreak( obj->meta );
					gcDecreaseRefCountThenBreak( obj->myClass );
					break;
				}
			case DAO_CDATA : case DAO_CTYPE :
				{
					DaoCData *cdata = (DaoCData*) value;
					gcDecreaseRefCountThenBreak( cdata->meta );
					gcDecreaseRefCountThenBreak( cdata->daoObject );
					gcDecreaseRefCountThenBreak( cdata->ctype );
					break;
				}
			case DAO_FUNCTREE :
				{
					DaoFunctree *meta = (DaoFunctree*) value;
					gcDecreaseRefCountThenBreak( meta->space );
					gcDecreaseRefCountThenBreak( meta->host );
					gcDecreaseRefCountThenBreak( meta->unitype );
					directRefCountDecrement( meta->routines );
					break;
				}
			case DAO_ROUTINE :
			case DAO_FUNCTION :
				{
					DaoRoutine *rout = (DaoRoutine*)value;
					gcDecreaseRefCountThenBreak( rout->nameSpace );
					gcDecreaseRefCountThenBreak( rout->routType );
					gcDecreaseRefCountThenBreak( rout->routHost );
					directRefCountDecrement( rout->routConsts );
					if( rout->type == DAO_ROUTINE ){
						gcDecreaseRefCountThenBreak( rout->upRoutine );
						gcDecreaseRefCountThenBreak( rout->upContext );
						gcDecreaseRefCountThenBreak( rout->original );
						gcDecreaseRefCountThenBreak( rout->specialized );
						directRefCountDecrement( rout->regType );
						directRefCountDecrementMapValue( rout->abstypes );
					}
					break;
				}
			case DAO_CLASS :
				{
					DaoClass *klass = (DaoClass*)value;
					gcDecreaseRefCountThenBreak( klass->clsType );
					gcDecreaseRefCountThenBreak( klass->classRoutine );
					directRefCountDecrementMapValue( klass->abstypes );
					directRefCountDecrement( klass->cstData );
					directRefCountDecrement( klass->glbData );
					directRefCountDecrement( klass->objDataDefault );
					directRefCountDecrement( klass->superClass );
					directRefCountDecrement( klass->objDataType );
					directRefCountDecrement( klass->glbDataType );
					directRefCountDecrement( klass->references );
					break;
				}
			case DAO_INTERFACE :
				{
					DaoInterface *inter = (DaoInterface*)value;
					directRefCountDecrementMapValue( inter->methods );
					directRefCountDecrement( inter->supers );
					gcDecreaseRefCountThenBreak( inter->abtype );
					break;
				}
			case DAO_CONTEXT :
				{
					DaoContext *ctx = (DaoContext*)value;
					gcDecreaseRefCountThenBreak( ctx->object );
					gcDecreaseRefCountThenBreak( ctx->routine );
					directRefCountDecrementT( ctx->regArray );
					break;
				}
			case DAO_NAMESPACE :
				{
					DaoNameSpace *ns = (DaoNameSpace*) value;
					directRefCountDecrement( ns->cstData );
					directRefCountDecrement( ns->varData );
					directRefCountDecrement( ns->varType );
					directRefCountDecrement( ns->cmodule->cmethods );
					directRefCountDecrement( ns->mainRoutines );
					directRefCountDecrementMapValue( ns->abstypes );
					for(k=0; k<ns->cmodule->ctypers->size; k++){
						DaoTypeBase *typer = (DaoTypeBase*)ns->cmodule->ctypers->items.pValue[k];
						if( typer->priv == NULL ) continue;
						directRefCountDecrementMapValue( typer->priv->values );
						directRefCountDecrementMapValue( typer->priv->methods );
					}
					break;
				}
			case DAO_TYPE :
				{
					DaoType *abtp = (DaoType*) value;
					directRefCountDecrement( abtp->nested );
					gcDecreaseRefCountThenBreak( abtp->aux );
					gcDecreaseRefCountThenBreak( abtp->value );
					if( abtp->interfaces ){
						node = DMap_First( abtp->interfaces );
						for( ; node != NULL; node = DMap_Next( abtp->interfaces, node ) )
							node->key.pValue->refCount --;
						DMap_Clear( abtp->interfaces );
					}
					break;
				}
			case DAO_FUTURE :
				{
					DaoFuture *future = (DaoFuture*) value;
					gcDecreaseRefCountThenBreak( future->value );
					gcDecreaseRefCountThenBreak( future->unitype );
					gcDecreaseRefCountThenBreak( future->context );
					gcDecreaseRefCountThenBreak( future->process );
					gcDecreaseRefCountThenBreak( future->precondition );
					break;
				}
			case DAO_VMPROCESS :
				{
					DaoVmProcess *vmp = (DaoVmProcess*) value;
					DaoVmFrame *frame = vmp->firstFrame;
					gcDecreaseRefCountThenBreak( vmp->returned );
					directRefCountDecrement( vmp->parResume );
					directRefCountDecrement( vmp->parYield );
					directRefCountDecrement( vmp->exceptions );
					while( frame ){
						if( frame->context ) frame->context->refCount --;
						frame->context = NULL;
						frame = frame->next;
					}
					break;
				}
			default: break;
			}
			DMutex_Unlock( & gcWorker.mutex_switch_heap );
		}
	}

	for( i=0; i<pool->size; i++ ){
		DaoValue *value = pool->items.pValue[i];
		if( value->cycRefCount==0 || value->refCount==0 ){
			if( value->refCount !=0 ){
				printf(" refCount not zero %i: %i\n", value->type, value->refCount );

#if DEBUG
				if( value->type == DAO_FUNCTION ){
					DaoFunction *func = (DaoFunction*)value;
					printf( "%s\n", func->routName->mbs );
				}else if( value->type == DAO_TYPE ){
					DaoType *func = (DaoType*)value;
					printf( "%s\n", func->name->mbs );
				}
#endif
				DMutex_Lock( & gcWorker.mutex_switch_heap );
				if( ! ( value->gcState[ idle ] & GC_IN_POOL ) ){
					value->gcState[ idle ] = GC_IN_POOL;
					DArray_Append( gcWorker.pool[idle], value );
				}
				DMutex_Unlock( & gcWorker.mutex_switch_heap );
				continue;
			}
			if( ! ( value->gcState[idle] & GC_IN_POOL ) ){
#ifdef DAO_GC_PROF
				ObjectProfile[value->type] --;
#endif
				/*
				   if( value->type <= DAO_VMPROCESS )
				   if( value->type == DAO_STRING ){
				   DaoString *s = (DaoString*) value;
				   if( s->chars->mbs && s->chars->mbs->refCount > 1 ){
				   printf( "delete mbstring!!! %i\n", s->chars->mbs->refCount );
				   }
				   if( s->chars->wcs && s->chars->wcs->refCount > 1 ){
				   printf( "delete wcstring!!! %i\n", s->chars->wcs->refCount );
				   }
				   }
				   if( value->type < DAO_STRING )
				   if( value->type != DAO_CONTEXT )
				 */
				if( value->type >= DAO_FUNCTREE && value->type <= DAO_FUNCTION )
					DaoCallbackData_DeleteByCallback( value );
				DaoCallbackData_DeleteByUserdata( value );
				typer = DaoValue_GetTyper( value );
				typer->Delete( value );
			}
		}
	}
#ifdef DAO_GC_PROF
#warning "-------------------- DAO_GC_PROF is turned on."
	printf("heap[idle] = %i;\theap[work] = %i\n", gcWorker.pool[ idle ]->size, gcWorker.pool[ work ]->size );
	printf("=======================================\n");
	//printf( "mbs count = %i\n", daoCountMBS );
	printf( "array count = %i\n", daoCountArray );
	for(i=0; i<100; i++){
		if( ObjectProfile[i] != 0 ){
			printf( "type = %3i; rest = %5i\n", i, ObjectProfile[i] );
		}
	}
#endif
	DArray_Clear( pool );
}
void cycRefCountDecrement( DaoValue *value )
{
	const short work = gcWorker.work;
	if( !value ) return;
	if( ! ( value->gcState[work] & GC_IN_POOL ) ){
		DArray_Append( gcWorker.pool[work], value );
		value->gcState[work] = GC_IN_POOL;
		value->cycRefCount = value->refCount;
	}
	value->cycRefCount --;

	if( value->cycRefCount<0 ){
		/*
		   printf("cycRefCount<0 : %i\n", value->type);
		 */
		value->cycRefCount = 0;
	}
}
void cycRefCountIncrement( DaoValue *value )
{
	const short work = gcWorker.work;
	if( value ){
		value->cycRefCount++;
		if( ! ( value->gcState[work] & GC_MARKED ) ){
			value->gcState[work] |= GC_MARKED;
			DArray_Append( gcWorker.objAlive, value );
		}
	}
}

#else

void GC_Lock(){}
void GC_Unlock(){}

/* Incremental Garbage Collector */
enum DaoGCWorkType
{
	GC_INIT_RC ,
	GC_DEC_RC ,
	GC_INC_RC ,
	GC_INC_RC2 ,
	GC_DIR_DEC_RC ,
	GC_FREE
};

static void InitRC();
static void directDecRC();
static void SwitchGC();
static void ContinueGC();

#include"daoVmspace.h"
extern DaoVmSpace *mainVmSpace;
void DaoGC_IncRC( DaoValue *p )
{
	const short work = gcWorker.work;
	if( ! p ) return;
#ifdef DEBUG_TRACE
	if( p == 0x736d010 ){
		print_trace();
	}
#endif
	if( p->refCount == 0 ){
		p->refCount ++;
		return;
	}

	p->refCount ++;
	p->cycRefCount ++;
	if( ! ( p->gcState[work] & GC_IN_POOL ) && gcWorker.workType == GC_INC_RC ){
		DArray_Append( gcWorker.pool[work], p );
		p->gcState[work] = GC_IN_POOL;
	}
}
static int counts = 100;
void DaoGC_DecRC( DaoValue *p )
{
	const short idle = gcWorker.idle;
	if( ! p ) return;

#ifdef DEBUG_TRACE
	if( p == 0x27aed90 ) print_trace();
#endif
#if 0
	if( p->type == DAO_TYPE ){
		DaoType *abtp = (DaoType*) p;
		if( abtp->tid == DAO_LIST )
			return;
	}

#include"assert.h"
	printf( "DaoGC_DecRC: %i\n", p->type );
	assert( p->type != 48 );
	printf( "here: %i %i\n", gcWorker.pool[ ! idle ]->size, gcWorker.pool[ idle ]->size );
#endif

	DaoGC_DecRC2( p, -1 );

	if( gcWorker.busy ) return;
	counts --;
	if( gcWorker.count < gcWorker.gcMax ){
		if( counts ) return;
		counts = 100;
	}else{
		if( counts ) return;
		counts = 10;
	}

	if( gcWorker.pool[ ! idle ]->size )
		ContinueGC();
	else if( gcWorker.pool[ idle ]->size > gcWorker.gcMin )
		SwitchGC();
}
void DaoGC_IncRCs( DArray *list )
{
	size_t i;
	DaoValue **data;
	if( list->size == 0 ) return;
	data = list->items.pValue;
	for( i=0; i<list->size; i++) DaoGC_IncRC( data[i] );
}
void DaoGC_DecRCs( DArray *list )
{
	size_t i;
	DaoValue **data;
	if( list == NULL || list->size == 0 ) return;
	data = list->items.pValue;
	for( i=0; i<list->size; i++) DaoGC_DecRC( data[i] );
}
void DaoGC_ShiftRC( DaoValue *up, DaoValue *down )
{
	if( up == down ) return;
	if( up ) DaoGC_IncRC( up );
	if( down ) DaoGC_DecRC( down );
}

void SwitchGC()
{
	if( gcWorker.busy ) return;
	gcWorker.work = gcWorker.idle;
	gcWorker.idle = ! gcWorker.work;
	gcWorker.workType = 0;
	gcWorker.ii = 0;
	gcWorker.jj = 0;
	ContinueGC();
}
void ContinueGC()
{
	if( gcWorker.busy ) return;
	gcWorker.busy = 1;
	switch( gcWorker.workType ){
	case GC_INIT_RC :
		DaoLateDeleter_Update();
		InitRC();
		break;
	case GC_DEC_RC :
		cycRefCountDecreScan();
		break;
	case GC_INC_RC :
	case GC_INC_RC2 :
		cycRefCountIncreScan();
		break;
	case GC_DIR_DEC_RC :
		directDecRC();
		break;
	case GC_FREE :
		freeGarbage();
		break;
	default : break;
	}
	gcWorker.busy = 0;
}
void DaoFinishGC()
{
	short idle = gcWorker.idle;
	while( gcWorker.pool[ idle ]->size || gcWorker.pool[ ! idle ]->size ){
		while( gcWorker.pool[ ! idle ]->size ) ContinueGC();
		if( gcWorker.pool[ idle ]->size ) SwitchGC();
		idle = gcWorker.idle;
		while( gcWorker.pool[ ! idle ]->size ) ContinueGC();
	}

	DArray_Delete( gcWorker.pool[0] );
	DArray_Delete( gcWorker.pool[1] );
	DArray_Delete( gcWorker.objAlive );
	DaoLateDeleter_Finish();
	gcWorker.objAlive = NULL;
}
void InitRC()
{
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	size_t i = gcWorker.ii;
	size_t k = gcWorker.ii + gcWorker.gcMin / 2;
	for( ; i<pool->size; i++ ){
		pool->items.pValue[i]->cycRefCount = pool->items.pValue[i]->refCount;
		if( i > k ) break;
	}
	if( i >= pool->size ){
		gcWorker.ii = 0;
		gcWorker.workType = GC_DEC_RC;
	}else{
		gcWorker.ii = i+1;
	}
}
void cycRefCountDecreScan()
{
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	DNode *node;
	size_t i = gcWorker.ii;
	size_t j = 0, k;

	for( ; i<pool->size; i++ ){
		DaoValue *value = pool->items.pValue[i];
		switch( value->type ){
#ifdef DAO_WITH_NUMARRAY
		case DAO_ARRAY :
			{
				DaoArray *array = (DaoArray*) value;
				cycRefCountDecrement( (DaoValue*) array->unitype );
				cycRefCountDecrement( (DaoValue*) array->meta );
				j ++;
				break;
			}
#endif
		case DAO_TUPLE :
			{
				DaoTuple *tuple = (DaoTuple*) value;
				cycRefCountDecrement( (DaoValue*) tuple->unitype );
				cycRefCountDecrement( (DaoValue*) tuple->meta );
				cycRefCountDecrementsT( tuple->items );
				j += tuple->items->size;
				break;
			}
		case DAO_LIST :
			{
				DaoList *list = (DaoList*) value;
				cycRefCountDecrement( (DaoValue*) list->unitype );
				cycRefCountDecrement( (DaoValue*) list->meta );
				cycRefCountDecrements( list->items );
				j += list->items->size;
				break;
			}
		case DAO_MAP :
			{
				DaoMap *map = (DaoMap*) value;
				cycRefCountDecrement( (DaoValue*) map->unitype );
				cycRefCountDecrement( (DaoValue*) map->meta );
				node = DMap_First( map->items );
				for( ; node != NULL; node = DMap_Next( map->items, node ) ) {
					cycRefCountDecrement( node->key.pValue );
					cycRefCountDecrement( node->value.pValue );
				}
				j += map->items->size;
				break;
			}
		case DAO_OBJECT :
			{
				DaoObject *obj = (DaoObject*) value;
				cycRefCountDecrementsT( obj->superObject );
				cycRefCountDecrementsT( obj->objData );
				if( obj->superObject ) j += obj->superObject->size;
				if( obj->objData ) j += obj->objData->size;
				cycRefCountDecrement( (DaoValue*) obj->meta );
				cycRefCountDecrement( (DaoValue*) obj->myClass );
				break;
			}
		case DAO_CDATA : case DAO_CTYPE :
			{
				DaoCData *cdata = (DaoCData*) value;
				cycRefCountDecrement( (DaoValue*) cdata->meta );
				cycRefCountDecrement( (DaoValue*) cdata->daoObject );
				cycRefCountDecrement( (DaoValue*) cdata->ctype );
				break;
			}
		case DAO_FUNCTREE :
			{
				DaoFunctree *meta = (DaoFunctree*) value;
				cycRefCountDecrement( (DaoValue*) meta->space );
				cycRefCountDecrement( (DaoValue*) meta->host );
				cycRefCountDecrement( (DaoValue*) meta->unitype );
				cycRefCountDecrements( meta->routines );
				break;
			}
		case DAO_ROUTINE :
		case DAO_FUNCTION :
			{
				DaoRoutine *rout = (DaoRoutine*)value;
				cycRefCountDecrement( (DaoValue*) rout->routType );
				cycRefCountDecrement( (DaoValue*) rout->routHost );
				cycRefCountDecrement( (DaoValue*) rout->nameSpace );
				cycRefCountDecrements( rout->routConsts );
				j += rout->routConsts->size;
				if( rout->type == DAO_ROUTINE ){
					j += rout->regType->size + rout->abstypes->size;
					cycRefCountDecrement( (DaoValue*) rout->upRoutine );
					cycRefCountDecrement( (DaoValue*) rout->upContext );
					cycRefCountDecrement( (DaoValue*) rout->original );
					cycRefCountDecrement( (DaoValue*) rout->specialized );
					cycRefCountDecrements( rout->regType );
					cycRefCountDecrementMapValue( rout->abstypes );
				}
				break;
			}
		case DAO_CLASS :
			{
				DaoClass *klass = (DaoClass*)value;
				cycRefCountDecrementMapValue( klass->abstypes );
				cycRefCountDecrement( (DaoValue*) klass->clsType );
				cycRefCountDecrement( (DaoValue*) klass->classRoutine );
				cycRefCountDecrements( klass->cstData );
				cycRefCountDecrements( klass->glbData );
				cycRefCountDecrements( klass->objDataDefault );
				cycRefCountDecrements( klass->superClass );
				cycRefCountDecrements( klass->objDataType );
				cycRefCountDecrements( klass->glbDataType );
				cycRefCountDecrements( klass->references );
				j += klass->cstData->size + klass->glbData->size;
				j += klass->cstData->size + klass->objDataDefault->size;
				j += klass->superClass->size + klass->abstypes->size;
				j += klass->objDataType->size + klass->glbDataType->size;
				j += klass->references->size + klass->abstypes->size;
				break;
			}
		case DAO_INTERFACE :
			{
				DaoInterface *inter = (DaoInterface*)value;
				cycRefCountDecrementMapValue( inter->methods );
				cycRefCountDecrements( inter->supers );
				cycRefCountDecrement( (DaoValue*) inter->abtype );
				j += inter->supers->size + inter->methods->size;
				break;
			}
		case DAO_CONTEXT :
			{
				DaoContext *ctx = (DaoContext*)value;
				cycRefCountDecrement( (DaoValue*) ctx->object );
				cycRefCountDecrement( (DaoValue*) ctx->routine );
				cycRefCountDecrementsT( ctx->regArray );
				j += ctx->regArray->size + 3;
				break;
			}
		case DAO_NAMESPACE :
			{
				DaoNameSpace *ns = (DaoNameSpace*) value;
				cycRefCountDecrements( ns->cstData );
				cycRefCountDecrements( ns->varData );
				cycRefCountDecrements( ns->varType );
				cycRefCountDecrements( ns->cmodule->cmethods );
				cycRefCountDecrements( ns->mainRoutines );
				j += ns->cstData->size + ns->varData->size + ns->abstypes->size;
				cycRefCountDecrementMapValue( ns->abstypes );
				for(k=0; k<ns->cmodule->ctypers->size; k++){
					DaoTypeBase *typer = (DaoTypeBase*)ns->cmodule->ctypers->items.pValue[k];
					if( typer->priv == NULL ) continue;
					cycRefCountDecrementMapValue( typer->priv->values );
					cycRefCountDecrementMapValue( typer->priv->methods );
				}
				break;
			}
		case DAO_TYPE :
			{
				DaoType *abtp = (DaoType*) value;
				cycRefCountDecrement( abtp->aux );
				cycRefCountDecrement( abtp->value );
				cycRefCountDecrements( abtp->nested );
				if( abtp->interfaces ){
					node = DMap_First( abtp->interfaces );
					for( ; node != NULL; node = DMap_Next( abtp->interfaces, node ) )
						cycRefCountDecrement( node->key.pValue );
				}
				break;
		case DAO_FUTURE :
			{
				DaoFuture *future = (DaoFuture*) value;
				cycRefCountDecrement( future->value );
				cycRefCountDecrement( (DaoValue*) future->unitype );
				cycRefCountDecrement( (DaoValue*) future->context );
				cycRefCountDecrement( (DaoValue*) future->process );
				cycRefCountDecrement( (DaoValue*) future->precondition );
				break;
			}
			}
		case DAO_VMPROCESS :
			{
				DaoVmProcess *vmp = (DaoVmProcess*) value;
				DaoVmFrame *frame = vmp->firstFrame;
				cycRefCountDecrement( vmp->returned );
				cycRefCountDecrements( vmp->parResume );
				cycRefCountDecrements( vmp->parYield );
				cycRefCountDecrements( vmp->exceptions );
				while( frame ){
					cycRefCountDecrement( (DaoValue*) frame->context );
					frame = frame->next;
				}
				break;
			}
		default: break;
		}
		if( j >= gcWorker.gcMin ) break;
	}
	if( i >= pool->size ){
		gcWorker.ii = 0;
		gcWorker.workType = GC_INC_RC;
	}else{
		gcWorker.ii = i+1;
	}
}
void cycRefCountIncreScan()
{
	const short work = gcWorker.work;
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	DNode *node;
	size_t i = gcWorker.ii;
	size_t j = 0, k;

	for( ; i<pool->size; i++ ){
		DaoValue *value = pool->items.pValue[i];
		j ++;
		if( value->type == DAO_CDATA && value->cycRefCount ==0 ){
			DaoCData *cdata = (DaoCData*) value;
			DaoCDataCore *core = (DaoCDataCore*)cdata->typer->priv;
			if( !(cdata->attribs & DAO_CDATA_FREE) ) continue;
			if( cdata->data == NULL || core == NULL ) continue;
			if( core->DelTest == NULL ) continue;
			if( core->DelTest( cdata->data ) ) continue;
			DaoCData_SetExtReference( cdata, 1 );
		}
		if( value->cycRefCount >0 && ! ( value->gcState[work] & GC_MARKED ) ){
			value->gcState[work] |= GC_MARKED;
			switch( value->type ){
#ifdef DAO_WITH_NUMARRAY
			case DAO_ARRAY :
				{
					DaoArray *array = (DaoArray*) value;
					cycRefCountIncrement( (DaoValue*) array->unitype );
					cycRefCountIncrement( (DaoValue*) array->meta );
					break;
				}
#endif
			case DAO_TUPLE :
				{
					DaoTuple *tuple= (DaoTuple*) value;
					cycRefCountIncrement( (DaoValue*) tuple->unitype );
					cycRefCountIncrement( (DaoValue*) tuple->meta );
					cycRefCountIncrementsT( tuple->items );
					j += tuple->items->size;
					break;
				}
			case DAO_LIST :
				{
					DaoList *list= (DaoList*) value;
					cycRefCountIncrement( (DaoValue*) list->unitype );
					cycRefCountIncrement( (DaoValue*) list->meta );
					cycRefCountIncrements( list->items );
					j += list->items->size;
					break;
				}
			case DAO_MAP :
				{
					DaoMap *map = (DaoMap*)value;
					cycRefCountIncrement( (DaoValue*) map->unitype );
					cycRefCountIncrement( (DaoValue*) map->meta );
					node = DMap_First( map->items );
					for( ; node != NULL; node = DMap_Next( map->items, node ) ){
						cycRefCountIncrement( node->key.pValue );
						cycRefCountIncrement( node->value.pValue );
					}
					j += map->items->size;
					break;
				}
			case DAO_OBJECT :
				{
					DaoObject *obj = (DaoObject*) value;
					cycRefCountIncrementsT( obj->superObject );
					cycRefCountIncrementsT( obj->objData );
					cycRefCountIncrement( (DaoValue*) obj->meta );
					cycRefCountIncrement( (DaoValue*) obj->myClass );
					if( obj->superObject ) j += obj->superObject->size;
					if( obj->objData ) j += obj->objData->size;
					break;
				}
			case DAO_CDATA : case DAO_CTYPE :
				{
					DaoCData *cdata = (DaoCData*) value;
					cycRefCountIncrement( (DaoValue*) cdata->meta );
					cycRefCountIncrement( (DaoValue*) cdata->daoObject );
					cycRefCountIncrement( (DaoValue*) cdata->ctype );
					break;
				}
		case DAO_FUNCTREE :
			{
				DaoFunctree *meta = (DaoFunctree*) value;
				cycRefCountIncrement( (DaoValue*) meta->space );
				cycRefCountIncrement( (DaoValue*) meta->host );
				cycRefCountIncrement( (DaoValue*) meta->unitype );
				cycRefCountIncrements( meta->routines );
				break;
			}
			case DAO_ROUTINE :
			case DAO_FUNCTION :
				{
					DaoRoutine *rout = (DaoRoutine*) value;
					cycRefCountIncrement( (DaoValue*) rout->routType );
					cycRefCountIncrement( (DaoValue*) rout->routHost );
					cycRefCountIncrement( (DaoValue*) rout->nameSpace );
					cycRefCountIncrements( rout->routConsts );
					if( rout->type == DAO_ROUTINE ){
						j += rout->abstypes->size;
						cycRefCountIncrement( (DaoValue*) rout->upRoutine );
						cycRefCountIncrement( (DaoValue*) rout->upContext );
						cycRefCountIncrement( (DaoValue*) rout->original );
						cycRefCountIncrement( (DaoValue*) rout->specialized );
						cycRefCountIncrements( rout->regType );
						cycRefCountIncrementMapValue( rout->abstypes );
					}
					j += rout->routConsts->size;
					break;
				}
			case DAO_CLASS :
				{
					DaoClass *klass = (DaoClass*) value;
					cycRefCountIncrementMapValue( klass->abstypes );
					cycRefCountIncrement( (DaoValue*) klass->clsType );
					cycRefCountIncrement( (DaoValue*) klass->classRoutine );
					cycRefCountIncrements( klass->cstData );
					cycRefCountIncrements( klass->glbData );
					cycRefCountIncrements( klass->objDataDefault );
					cycRefCountIncrements( klass->superClass );
					cycRefCountIncrements( klass->objDataType );
					cycRefCountIncrements( klass->glbDataType );
					cycRefCountIncrements( klass->references );
					j += klass->cstData->size + klass->glbData->size;
					j += klass->cstData->size + klass->objDataDefault->size;
					j += klass->superClass->size + klass->abstypes->size;
					j += klass->objDataType->size + klass->glbDataType->size;
					j += klass->references->size + klass->abstypes->size;
					break;
				}
			case DAO_INTERFACE :
				{
					DaoInterface *inter = (DaoInterface*)value;
					cycRefCountIncrementMapValue( inter->methods );
					cycRefCountIncrements( inter->supers );
					cycRefCountIncrement( (DaoValue*) inter->abtype );
					j += inter->supers->size + inter->methods->size;
					break;
				}
			case DAO_CONTEXT :
				{
					DaoContext *ctx = (DaoContext*)value;
					cycRefCountIncrement( (DaoValue*) ctx->object );
					cycRefCountIncrement( (DaoValue*) ctx->routine );
					cycRefCountIncrementsT( ctx->regArray );
					j += ctx->regArray->size + 3;
					break;
				}
			case DAO_NAMESPACE :
				{
					DaoNameSpace *ns = (DaoNameSpace*) value;
					cycRefCountIncrements( ns->cstData );
					cycRefCountIncrements( ns->varData );
					cycRefCountIncrements( ns->varType );
					cycRefCountIncrements( ns->cmodule->cmethods );
					cycRefCountIncrements( ns->mainRoutines );
					j += ns->cstData->size + ns->varData->size + ns->abstypes->size;
					cycRefCountIncrementMapValue( ns->abstypes );
					for(k=0; k<ns->cmodule->ctypers->size; k++){
						DaoTypeBase *typer = (DaoTypeBase*)ns->cmodule->ctypers->items.pValue[k];
						if( typer->priv == NULL ) continue;
						cycRefCountIncrementMapValue( typer->priv->values );
						cycRefCountIncrementMapValue( typer->priv->methods );
					}
					break;
				}
			case DAO_TYPE :
				{
					DaoType *abtp = (DaoType*) value;
					cycRefCountIncrement( abtp->aux );
					cycRefCountIncrement( abtp->value );
					cycRefCountIncrements( abtp->nested );
					if( abtp->interfaces ){
						node = DMap_First( abtp->interfaces );
						for( ; node != NULL; node = DMap_Next( abtp->interfaces, node ) )
							cycRefCountIncrement( node->key.pValue );
					}
					break;
				}
			case DAO_FUTURE :
				{
					DaoFuture *future = (DaoFuture*) value;
					cycRefCountIncrement( future->value );
					cycRefCountIncrement( (DaoValue*) future->unitype );
					cycRefCountIncrement( (DaoValue*) future->context );
					cycRefCountIncrement( (DaoValue*) future->process );
					cycRefCountIncrement( (DaoValue*) future->precondition );
					break;
				}
			case DAO_VMPROCESS :
				{
					DaoVmProcess *vmp = (DaoVmProcess*) value;
					DaoVmFrame *frame = vmp->firstFrame;
					cycRefCountIncrement( vmp->returned );
					cycRefCountIncrements( vmp->parResume );
					cycRefCountIncrements( vmp->parYield );
					cycRefCountIncrements( vmp->exceptions );
					while( frame ){
						cycRefCountIncrement( (DaoValue*) frame->context );
						frame = frame->next;
					}
					break;
				}
			default: break;
			}
		}
		if( j >= gcWorker.gcMin ) break;
	}
	if( i >= pool->size ){
		gcWorker.ii = 0;
		gcWorker.workType ++;
		gcWorker.boundary = pool->size;
	}else{
		gcWorker.ii = i+1;
	}
}
void directDecRC()
{
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	DNode *node;
	const short work = gcWorker.work;
	size_t boundary = gcWorker.boundary;
	size_t i = gcWorker.ii;
	size_t j = 0, k;

	for( ; i<boundary; i++ ){
		DaoValue *value = pool->items.pValue[i];
		value->gcState[work] = 0;
		j ++;
		if( value->cycRefCount == 0 ){

			switch( value->type ){

#ifdef DAO_WITH_NUMARRAY
			case DAO_ARRAY :
				{
					DaoArray *array = (DaoArray*) value;
					gcDecreaseRefCountThenBreak( array->unitype );
					gcDecreaseRefCountThenBreak( array->meta );
					break;
				}
#endif
			case DAO_TUPLE :
				{
					DaoTuple *tuple = (DaoTuple*) value;
					j += tuple->items->size;
					directRefCountDecrementT( tuple->items );
					gcDecreaseRefCountThenBreak( tuple->unitype );
					gcDecreaseRefCountThenBreak( tuple->meta );
					break;
				}
			case DAO_LIST :
				{
					DaoList *list = (DaoList*) value;
					j += list->items->size;
					directRefCountDecrement( list->items );
					gcDecreaseRefCountThenBreak( list->unitype );
					gcDecreaseRefCountThenBreak( list->meta );
					break;
				}
			case DAO_MAP :
				{
					DaoMap *map = (DaoMap*) value;
					node = DMap_First( map->items );
					for( ; node != NULL; node = DMap_Next( map->items, node ) ){
						node->key.pValue->refCount --;
						node->value.pValue->refCount --;
						node->key.pValue = NULL;
						node->value.pValue = NULL;
					}
					j += map->items->size;
					DMap_Clear( map->items );
					gcDecreaseRefCountThenBreak( map->unitype );
					gcDecreaseRefCountThenBreak( map->meta );
					break;
				}
			case DAO_OBJECT :
				{
					DaoObject *obj = (DaoObject*) value;
					if( obj->superObject ) j += obj->superObject->size;
					if( obj->objData ) j += obj->objData->size;
					directRefCountDecrementT( obj->superObject );
					directRefCountDecrementT( obj->objData );
					gcDecreaseRefCountThenBreak( obj->meta );
					gcDecreaseRefCountThenBreak( obj->myClass );
					break;
				}
			case DAO_CDATA : case DAO_CTYPE :
				{
					DaoCData *cdata = (DaoCData*) value;
					gcDecreaseRefCountThenBreak( cdata->meta );
					gcDecreaseRefCountThenBreak( cdata->daoObject );
					gcDecreaseRefCountThenBreak( cdata->ctype );
					break;
				}
			case DAO_FUNCTREE :
				{
					DaoFunctree *meta = (DaoFunctree*) value;
					j += meta->routines->size;
					gcDecreaseRefCountThenBreak( meta->space );
					gcDecreaseRefCountThenBreak( meta->host );
					gcDecreaseRefCountThenBreak( meta->unitype );
					directRefCountDecrement( meta->routines );
					break;
				}
			case DAO_ROUTINE :
			case DAO_FUNCTION :
				{
					DaoRoutine *rout = (DaoRoutine*)value;
					gcDecreaseRefCountThenBreak( rout->nameSpace );
					/* may become NULL, if it has already become garbage 
					 * in the last cycle */
					gcDecreaseRefCountThenBreak( rout->routType );
					/* may become NULL, if it has already become garbage 
					 * in the last cycle */
					gcDecreaseRefCountThenBreak( rout->routHost );

					j += rout->routConsts->size;
					directRefCountDecrement( rout->routConsts );
					if( rout->type == DAO_ROUTINE ){
						j += rout->abstypes->size;
						gcDecreaseRefCountThenBreak( rout->upRoutine );
						gcDecreaseRefCountThenBreak( rout->upContext );
						gcDecreaseRefCountThenBreak( rout->original );
						gcDecreaseRefCountThenBreak( rout->specialized );
						directRefCountDecrement( rout->regType );
						directRefCountDecrementMapValue( rout->abstypes );
					}
					break;
				}
			case DAO_CLASS :
				{
					DaoClass *klass = (DaoClass*)value;
					j += klass->cstData->size + klass->glbData->size;
					j += klass->cstData->size + klass->objDataDefault->size;
					j += klass->superClass->size + klass->abstypes->size;
					j += klass->objDataType->size + klass->glbDataType->size;
					j += klass->references->size + klass->abstypes->size;
					gcDecreaseRefCountThenBreak( klass->clsType );
					gcDecreaseRefCountThenBreak( klass->classRoutine );
					directRefCountDecrementMapValue( klass->abstypes );
					directRefCountDecrement( klass->cstData );
					directRefCountDecrement( klass->glbData );
					directRefCountDecrement( klass->objDataDefault );
					directRefCountDecrement( klass->superClass );
					directRefCountDecrement( klass->objDataType );
					directRefCountDecrement( klass->glbDataType );
					directRefCountDecrement( klass->references );
					break;
				}
			case DAO_INTERFACE :
				{
					DaoInterface *inter = (DaoInterface*)value;
					j += inter->supers->size + inter->methods->size;
					directRefCountDecrementMapValue( inter->methods );
					directRefCountDecrement( inter->supers );
					gcDecreaseRefCountThenBreak( inter->abtype );
					break;
				}
			case DAO_CONTEXT :
				{
					DaoContext *ctx = (DaoContext*)value;
					gcDecreaseRefCountThenBreak( ctx->object );
					gcDecreaseRefCountThenBreak( ctx->routine );
					j += ctx->regArray->size + 3;
					directRefCountDecrementT( ctx->regArray );
					break;
				}
			case DAO_NAMESPACE :
				{
					DaoNameSpace *ns = (DaoNameSpace*) value;
					j += ns->cstData->size + ns->varData->size + ns->abstypes->size;
					directRefCountDecrement( ns->cstData );
					directRefCountDecrement( ns->varData );
					directRefCountDecrement( ns->varType );
					directRefCountDecrement( ns->cmodule->cmethods );
					directRefCountDecrement( ns->mainRoutines );
					directRefCountDecrementMapValue( ns->abstypes );
					for(k=0; k<ns->cmodule->ctypers->size; k++){
						DaoTypeBase *typer = (DaoTypeBase*)ns->cmodule->ctypers->items.pValue[k];
						if( typer->priv == NULL ) continue;
						directRefCountDecrementMapValue( typer->priv->values );
						directRefCountDecrementMapValue( typer->priv->methods );
					}
					break;
				}
			case DAO_TYPE :
				{
					DaoType *abtp = (DaoType*) value;
					directRefCountDecrement( abtp->nested );
					gcDecreaseRefCountThenBreak( abtp->aux );
					gcDecreaseRefCountThenBreak( abtp->value );
					if( abtp->interfaces ){
						node = DMap_First( abtp->interfaces );
						for( ; node != NULL; node = DMap_Next( abtp->interfaces, node ) )
							node->key.pValue->refCount --;
						DMap_Clear( abtp->interfaces );
					}
					break;
				}
			case DAO_FUTURE :
				{
					DaoFuture *future = (DaoFuture*) value;
					gcDecreaseRefCountThenBreak( future->value );
					gcDecreaseRefCountThenBreak( future->unitype );
					gcDecreaseRefCountThenBreak( future->context );
					gcDecreaseRefCountThenBreak( future->process );
					gcDecreaseRefCountThenBreak( future->precondition );
					break;
				}
			case DAO_VMPROCESS :
				{
					DaoVmProcess *vmp = (DaoVmProcess*) value;
					DaoVmFrame *frame = vmp->firstFrame;
					gcDecreaseRefCountThenBreak( vmp->returned );
					directRefCountDecrement( vmp->parResume );
					directRefCountDecrement( vmp->parYield );
					directRefCountDecrement( vmp->exceptions );
					while( frame ){
						if( frame->context ) frame->context->refCount --;
						frame->context = NULL;
						frame = frame->next;
					}
					break;
				}
			default: break;
			}
		}
		if( j >= gcWorker.gcMin ) break;
	}
	if( i >= boundary ){
		gcWorker.ii = 0;
		gcWorker.workType = GC_FREE;
	}else{
		gcWorker.ii = i+1;
	}
}

void freeGarbage()
{
	DArray *pool = gcWorker.pool[ gcWorker.work ];
	DaoTypeBase *typer;
	const short work = gcWorker.work;
	const short idle = gcWorker.idle;
	size_t boundary = gcWorker.boundary;
	size_t i = gcWorker.ii;
	size_t j = 0;

	for( ; i<boundary; i++ ){
		DaoValue *value = pool->items.pValue[i];
		value->gcState[work] = 0;
		j ++;
		if( value->cycRefCount==0 ){
			if( value->refCount !=0 ){
				printf(" refCount not zero %p %i: %i, %i\n", value, value->type, value->refCount, value->trait);
#if DEBUG
				if( value->type == DAO_FUNCTION ){
					DaoFunction *func = (DaoFunction*)value;
					printf( "%s\n", func->routName->mbs );
				}else if( value->type == DAO_TYPE ){
					DaoType *func = (DaoType*)value;
					printf( "%s\n", func->name->mbs );
				}else if( value->type == DAO_CDATA ){
					DaoCData *cdata = (DaoCData*) value;
					printf( "%s\n", cdata->typer->name );
				}
#endif
				if( ! ( value->gcState[ idle ] & GC_IN_POOL ) ){
					value->gcState[ idle ] = GC_IN_POOL;
					DArray_Append( gcWorker.pool[idle], value );
				}
				continue;
			}
			if( ! ( value->gcState[idle] & GC_IN_POOL ) ){
#ifdef DAO_GC_PROF
				ObjectProfile[value->type] --;
#endif
				/*
				   if( value->type <= DAO_VMPROCESS )
				   if( value->type == DAO_STRING ){
				   DaoString *s = (DaoString*) value;
				   if( s->chars->mbs && s->chars->mbs->refCount > 1 ){
				   printf( "delete mbstring!!! %i\n", s->chars->mbs->refCount );
				   }
				   if( s->chars->wcs && s->chars->wcs->refCount > 1 ){
				   printf( "delete wcstring!!! %i\n", s->chars->wcs->refCount );
				   }
				   }
				   if( value->type == DAO_FUNCTION ) printf( "here\n" );
				   if( value->type < DAO_STRING )
				 */
				if( value->type >= DAO_FUNCTREE && value->type <= DAO_FUNCTION )
					DaoCallbackData_DeleteByCallback( value );
				DaoCallbackData_DeleteByUserdata( value );
				typer = DaoValue_GetTyper( value );
				typer->Delete( value );
			}
		}
		if( j >= gcWorker.gcMin ) break;
	}
#ifdef DAO_GC_PROF
	printf("heap[idle] = %i;\theap[work] = %i\n", gcWorker.pool[ idle ]->size, gcWorker.pool[ work ]->size );
	printf("=======================================\n");
	//printf( "mbs count = %i\n", daoCountMBS );
	printf( "array count = %i\n", daoCountArray );
	int k;
	for(k=0; k<100; k++){
		if( ObjectProfile[k] > 0 ){
			printf( "type = %3i; rest = %5i\n", k, ObjectProfile[k] );
		}
	}
#endif
	if( i >= boundary ){
		gcWorker.ii = 0;
		gcWorker.workType = 0;
		gcWorker.count = 0;
		DArray_Clear( pool );
	}else{
		gcWorker.ii = i+1;
	}
}
void cycRefCountDecrement( DaoValue *value )
{
	const short work = gcWorker.work;
	if( !value ) return;
	if( ! ( value->gcState[work] & GC_IN_POOL ) ){
		DArray_Append( gcWorker.pool[work], value );
		value->gcState[work] = GC_IN_POOL;
		value->cycRefCount = value->refCount;
	}
	value->cycRefCount --;

	if( value->cycRefCount<0 ){
		/*
		   printf("cycRefCount<0 : %i\n", value->type);
		 */
		value->cycRefCount = 0;
	}
}
void cycRefCountIncrement( DaoValue *value )
{
	const short work = gcWorker.work;
	if( value ){
		value->cycRefCount++;
		if( ! ( value->gcState[work] & GC_MARKED ) ){
			DArray_Append( gcWorker.pool[work], value );
		}
	}
}
#endif
void cycRefCountDecrements( DArray *list )
{
	DaoValue **values;
	size_t i;
	if( list == NULL ) return;
	values = list->items.pValue;
	for( i=0; i<list->size; i++ ) cycRefCountDecrement( values[i] );
}
void cycRefCountIncrements( DArray *list )
{
	DaoValue **values;
	size_t i;
	if( list == NULL ) return;
	values = list->items.pValue;
	for( i=0; i<list->size; i++ ) cycRefCountIncrement( values[i] );
}
void directRefCountDecrement( DArray *list )
{
	DaoValue **values;
	size_t i;
	if( list == NULL ) return;
	values = list->items.pValue;
	for( i=0; i<list->size; i++ ) if( values[i] ) values[i]->refCount --;
	DArray_Clear( list );
}
void cycRefCountDecrementMapValue( DMap *dmap )
{
	DNode *it;
	if( dmap == NULL ) return;
	for( it = DMap_First( dmap ); it != NULL; it = DMap_Next( dmap, it ) )
		cycRefCountDecrement( it->value.pValue );
}
void cycRefCountIncrementMapValue( DMap *dmap )
{
	DNode *it;
	if( dmap == NULL ) return;
	for( it = DMap_First( dmap ); it != NULL; it = DMap_Next( dmap, it ) )
		cycRefCountIncrement( it->value.pValue );
}
void directRefCountDecrementMapValue( DMap *dmap )
{
	DNode *it;
	if( dmap == NULL ) return;
	for( it = DMap_First( dmap ); it != NULL; it = DMap_Next( dmap, it ) )
		it->value.pValue->refCount --;
	DMap_Clear( dmap );
}
void cycRefCountDecrementsT( DTuple *list )
{
	size_t i;
	DaoValue **values;
	if( list ==NULL ) return;
	values = list->items.pValue;
	for( i=0; i<list->size; i++ ) cycRefCountDecrement( values[i] );
}
void cycRefCountIncrementsT( DTuple *list )
{
	size_t i;
	DaoValue **values;
	if( list ==NULL ) return;
	values = list->items.pValue;
	for( i=0; i<list->size; i++ ) cycRefCountIncrement( values[i] );
}
void directRefCountDecrementT( DTuple *list )
{
	size_t i;
	DaoValue **values;
	if( list ==NULL ) return;
	values = list->items.pValue;
	for( i=0; i<list->size; i++ ) if( values[i] ) values[i]->refCount --;
	DTuple_Clear( list );
}

DaoLateDeleter dao_late_deleter;
#ifdef DAO_WITH_THREAD
DMutex dao_late_deleter_mutex;
#endif

void DaoLateDeleter_Init()
{
	dao_late_deleter.lock = 0;
	dao_late_deleter.safe = 1;
	dao_late_deleter.version = 0;
	dao_late_deleter.buffer = DArray_New(0);
#ifdef DAO_WITH_THREAD
	DMutex_Init( & dao_late_deleter_mutex );
#endif
}
void DaoLateDeleter_Finish()
{
	dao_late_deleter.safe = 0;
	dao_late_deleter.lock = 0;
	DaoLateDeleter_Update();
	DArray_Delete( dao_late_deleter.buffer );
}
void DaoLateDeleter_Push( void *p )
{
#ifdef DAO_WITH_THREAD
	DMutex_Lock( & dao_late_deleter_mutex );
#endif
	DArray_Append( dao_late_deleter.buffer, p );
#ifdef DAO_WITH_THREAD
	DMutex_Unlock( & dao_late_deleter_mutex );
#endif
}
void DaoLateDeleter_Update()
{
	DaoLateDeleter *self = & dao_late_deleter;
	DArray *buffer = self->buffer;
	size_t i;
	switch( (self->safe<<1)|self->lock ){
	case 2 : /* safe=1, lock=0 */
		if( self->buffer->size < 10000 ) break;
		self->safe = 0;
		self->lock = 0;
		self->version += 1;
		break;
	case 0 : /* safe=0, lock=0 */
		self->lock = 1;
#ifdef DAO_WITH_THREAD
		DMutex_Lock( & dao_late_deleter_mutex );
#endif
		for(i=0; i<buffer->size; i++) dao_free( buffer->items.pVoid[i] );
		buffer->size = 0;
#ifdef DAO_WITH_THREAD
		DMutex_Unlock( & dao_late_deleter_mutex );
#endif
		break;
	case 1 : /* safe=0, lock=1 */
		self->safe = 1;
		self->lock = 0;
		break;
	default : break;
	}
}
