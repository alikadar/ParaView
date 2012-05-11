/*=========================================================================

  Program:   ParaView
  Module:    $RCSfile$

  Copyright (c) Kitware, Inc.
  All rights reserved.
  See Copyright.txt or http://www.paraview.org/HTML/Copyright.html for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkSMDataDeliveryManager.h"

#include "vtkCamera.h"
#include "vtkClientServerStream.h"
#include "vtkCommand.h"
#include "vtkObjectFactory.h"
#include "vtkObjectFactory.h"
#include "vtkPVRenderView.h"
#include "vtkRenderer.h"
#include "vtkRepresentedDataStorage.h"
#include "vtkSMSession.h"
#include "vtkSMViewProxy.h"
#include "vtkTimerLog.h"

#include <assert.h>

vtkStandardNewMacro(vtkSMDataDeliveryManager);
//----------------------------------------------------------------------------
vtkSMDataDeliveryManager::vtkSMDataDeliveryManager():
  UpdateObserverTag(0)
{
}

//----------------------------------------------------------------------------
vtkSMDataDeliveryManager::~vtkSMDataDeliveryManager()
{
  if (this->ViewProxy && this->UpdateObserverTag)
    {
    this->ViewProxy->RemoveObserver(this->UpdateObserverTag);
    this->UpdateObserverTag = 0;
    }
}

//----------------------------------------------------------------------------
void vtkSMDataDeliveryManager::SetViewProxy(vtkSMViewProxy* viewproxy)
{
  if (viewproxy == this->ViewProxy)
    {
    return;
    }

  if (this->ViewProxy && this->UpdateObserverTag)
    {
    this->ViewProxy->RemoveObserver(this->UpdateObserverTag);
    this->UpdateObserverTag = 0;
    }
  this->ViewProxy = viewproxy;
  if (this->ViewProxy)
    {
    this->UpdateObserverTag = this->ViewProxy->AddObserver(
      vtkCommand::UpdateDataEvent,
      this, &vtkSMDataDeliveryManager::OnViewUpdated);
    }

  this->Modified();
  this->ViewUpdateStamp.Modified();
}

//----------------------------------------------------------------------------
void vtkSMDataDeliveryManager::OnViewUpdated()
{
  // Update the timestamp so we realize that view was updated. Hence, we can
  // expect new geometry needs to be delivered. Otherwise that code is
  // short-circuited for better rendering performance.
  this->ViewUpdateStamp.Modified();
}

//----------------------------------------------------------------------------
void vtkSMDataDeliveryManager::Deliver(bool interactive)
{
  assert(this->ViewProxy != NULL);

  vtkPVRenderView* view = vtkPVRenderView::SafeDownCast(
    this->ViewProxy->GetClientSideObject());
  bool use_lod = interactive && view->GetUseLODForInteractiveRender();

  // Delivery the "base" geometries for all representations. This is true,
  // irrespective of whether we are streaming datasets. When a representation is
  // updated, it provides a base-geometry that gets delivered for rendering (if
  // any). This code handles that.
  if ( (!use_lod && this->ViewUpdateStamp < this->GeometryDeliveryStamp) ||
    (use_lod && this->ViewUpdateStamp < this->LODGeometryDeliveryStamp) )
    {
    return;
    }

  vtkTimeStamp& timeStamp = use_lod?
    this->LODGeometryDeliveryStamp : this->GeometryDeliveryStamp;

  std::vector<unsigned int> keys_to_deliver;
  
  // FIXME:STREAMING this logic is not the best here. We end up re-delivering
  // geometry when the representation didn't really update at all.
  if (!view->GetGeometryStore()->NeedsDelivery(timeStamp, keys_to_deliver, use_lod))
    {
    return;
    }

  cout << "Request Delivery: " <<  keys_to_deliver.size() << endl;
  vtkClientServerStream stream;
  stream << vtkClientServerStream::Invoke
         << VTKOBJECT(this->ViewProxy)
         << "GetGeometryStore"
         << vtkClientServerStream::End;
  stream << vtkClientServerStream::Invoke
         << vtkClientServerStream::LastResult
         << "Deliver"
         << static_cast<int>(use_lod)
         << static_cast<unsigned int>(keys_to_deliver.size())
         << vtkClientServerStream::InsertArray(
           &keys_to_deliver[0], keys_to_deliver.size())
         << vtkClientServerStream::End;
  this->ViewProxy->GetSession()->ExecuteStream(
    this->ViewProxy->GetLocation(), stream, false);
  timeStamp.Modified();
}

//----------------------------------------------------------------------------
bool vtkSMDataDeliveryManager::DeliverNextPiece()
{
  // This method gets called to deliver next piece in queue during streaming.
  // vtkSMDataDeliveryManager::Deliver() relies on the "client" to decide what
  // representations are dirty and requests geometries for those. This makes it
  // possible to work well in multi-clients mode without putting any additional
  // burden on the server side.
  //
  // For streaming, however, we require that multi-clients mode is disabled and
  // the server decides what pieces need to be delivered since the server has
  // more information about the data to decide how it should be streamed.

  vtkTimerLog::MarkStartEvent("DeliverNextPiece");
  vtkSMSession* session = this->ViewProxy->GetSession();

  vtkPVRenderView* view = vtkPVRenderView::SafeDownCast(
    this->ViewProxy->GetClientSideObject());

  double planes[24];
  vtkRenderer* ren = view->GetRenderer();
  ren->GetActiveCamera()->GetFrustumPlanes(
    ren->GetTiledAspectRatio(), planes);

  vtkClientServerStream stream;
  stream << vtkClientServerStream::Invoke
         << VTKOBJECT(this->ViewProxy)
         << "GetNextPieceToDeliver"
         << vtkClientServerStream::InsertArray(planes, 24)
         << vtkClientServerStream::End;
  session->ExecuteStream(vtkPVSession::DATA_SERVER_ROOT, stream, false);

  const vtkClientServerStream& result = session->GetLastResult(
    vtkPVSession::DATA_SERVER_ROOT);
  //result.Print(cout);

  // extract the "piece-key" from the result and then request it.
  unsigned int representation_id = 0;
  result.GetArgument(0, 0, &representation_id);
  if (representation_id != 0)
    {
    // we have something to deliver.
    vtkClientServerStream stream;
    stream << vtkClientServerStream::Invoke
           << VTKOBJECT(this->ViewProxy)
           << "GetGeometryStore"
           << vtkClientServerStream::End;
    stream << vtkClientServerStream::Invoke
           << vtkClientServerStream::LastResult
           << "StreamingDeliver"
           << representation_id 
           << vtkClientServerStream::End;
    this->ViewProxy->GetSession()->ExecuteStream(
      this->ViewProxy->GetLocation(), stream, false);
    }
  vtkTimerLog::MarkEndEvent("DeliverNextPiece");
  return representation_id != 0;
}

//----------------------------------------------------------------------------
void vtkSMDataDeliveryManager::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
