/***************************************************************************
    qgswfscapabilities.cpp
    ---------------------
    begin                : October 2011
    copyright            : (C) 2011 by Martin Dobias
                           (C) 2016 by Even Rouault
    email                : wonder dot sk at gmail dot com
                           even.rouault at spatialys.com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgswfscapabilities.h"
#include "qgswfsconstants.h"
#include "qgswfsutils.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgsogcutils.h"

#include <QDomDocument>
#include <QSettings>
#include <QStringList>

QgsWfsCapabilities::QgsWfsCapabilities( const QString& theUri )
    : QgsWfsRequest( theUri )
{
  connect( this, SIGNAL( downloadFinished() ), this, SLOT( capabilitiesReplyFinished() ) );
}

QgsWfsCapabilities::~QgsWfsCapabilities()
{
}

bool QgsWfsCapabilities::requestCapabilities( bool synchronous, bool forceRefresh )
{
  QUrl url( baseURL() );
  url.addQueryItem( QStringLiteral( "REQUEST" ), QStringLiteral( "GetCapabilities" ) );

  const QString& version = mUri.version();
  if ( version == QgsWFSConstants::VERSION_AUTO )
    // MapServer honours the order with the first value being the preferred one
    url.addQueryItem( QStringLiteral( "ACCEPTVERSIONS" ), QStringLiteral( "2.0.0,1.1.0,1.0.0" ) );
  else
    url.addQueryItem( QStringLiteral( "VERSION" ), version );

  if ( !sendGET( url, synchronous, forceRefresh ) )
  {
    emit gotCapabilities();
    return false;
  }
  return true;
}

QgsWfsCapabilities::Capabilities::Capabilities()
{
  clear();
}

void QgsWfsCapabilities::Capabilities::clear()
{
  maxFeatures = 0;
  supportsHits = false;
  supportsPaging = false;
  supportsJoins = false;
  version = QLatin1String( "" );
  featureTypes.clear();
  spatialPredicatesList.clear();
  functionList.clear();
  setAllTypenames.clear();
  mapUnprefixedTypenameToPrefixedTypename.clear();
  setAmbiguousUnprefixedTypename.clear();
  useEPSGColumnFormat = false;
}

QString QgsWfsCapabilities::Capabilities::addPrefixIfNeeded( const QString& name ) const
{
  if ( name.contains( ':' ) )
    return name;
  if ( setAmbiguousUnprefixedTypename.contains( name ) )
    return QLatin1String( "" );
  return mapUnprefixedTypenameToPrefixedTypename[name];
}

void QgsWfsCapabilities::capabilitiesReplyFinished()
{
  const QByteArray& buffer = mResponse;

  QgsDebugMsg( "parsing capabilities: " + buffer );

  // parse XML
  QString capabilitiesDocError;
  QDomDocument capabilitiesDocument;
  if ( !capabilitiesDocument.setContent( buffer, true, &capabilitiesDocError ) )
  {
    mErrorCode = QgsWfsRequest::XmlError;
    mErrorMessage = capabilitiesDocError;
    emit gotCapabilities();
    return;
  }

  QDomElement doc = capabilitiesDocument.documentElement();

  // handle exceptions
  if ( doc.tagName() == QLatin1String( "ExceptionReport" ) )
  {
    QDomNode ex = doc.firstChild();
    QString exc = ex.toElement().attribute( QStringLiteral( "exceptionCode" ), QStringLiteral( "Exception" ) );
    QDomElement ext = ex.firstChild().toElement();
    mErrorCode = QgsWfsRequest::ServerExceptionError;
    mErrorMessage = exc + ": " + ext.firstChild().nodeValue();
    emit gotCapabilities();
    return;
  }

  mCaps.clear();

  //test wfs version
  mCaps.version = doc.attribute( QStringLiteral( "version" ) );
  if ( !mCaps.version.startsWith( QLatin1String( "1.0" ) ) &&
       !mCaps.version.startsWith( QLatin1String( "1.1" ) ) &&
       !mCaps.version.startsWith( QLatin1String( "2.0" ) ) )
  {
    mErrorCode = WFSVersionNotSupported;
    mErrorMessage = tr( "WFS version %1 not supported" ).arg( mCaps.version );
    emit gotCapabilities();
    return;
  }

  // WFS 2.0 implementation are supposed to implement resultType=hits, and some
  // implementations (GeoServer) might advertize it, whereas others (MapServer) do not.
  // WFS 1.1 implementation too I think, but in the examples of the GetCapabilities
  // response of the WFS 1.1 standard (and in common implementations), this is
  // explicitly advertized
  if ( mCaps.version.startsWith( QLatin1String( "2.0" ) ) )
    mCaps.supportsHits = true;

  // Note: for conveniency, we do not use the elementsByTagNameNS() method as
  // the WFS and OWS namespaces URI are not the same in all versions

  // find <ows:OperationsMetadata>
  QDomElement operationsMetadataElem = doc.firstChildElement( QStringLiteral( "OperationsMetadata" ) );
  if ( !operationsMetadataElem.isNull() )
  {
    QDomNodeList contraintList = operationsMetadataElem.elementsByTagName( QStringLiteral( "Constraint" ) );
    for ( int i = 0; i < contraintList.size(); ++i )
    {
      QDomElement contraint = contraintList.at( i ).toElement();
      if ( contraint.attribute( QStringLiteral( "name" ) ) == QLatin1String( "DefaultMaxFeatures" ) /* WFS 1.1 */ )
      {
        QDomElement value = contraint.firstChildElement( QStringLiteral( "Value" ) );
        if ( !value.isNull() )
        {
          mCaps.maxFeatures = value.text().toInt();
          QgsDebugMsg( QString( "maxFeatures: %1" ).arg( mCaps.maxFeatures ) );
        }
      }
      else if ( contraint.attribute( QStringLiteral( "name" ) ) == QLatin1String( "CountDefault" ) /* WFS 2.0 (e.g. MapServer) */ )
      {
        QDomElement value = contraint.firstChildElement( QStringLiteral( "DefaultValue" ) );
        if ( !value.isNull() )
        {
          mCaps.maxFeatures = value.text().toInt();
          QgsDebugMsg( QString( "maxFeatures: %1" ).arg( mCaps.maxFeatures ) );
        }
      }
      else if ( contraint.attribute( QStringLiteral( "name" ) ) == QLatin1String( "ImplementsResultPaging" ) /* WFS 2.0 */ )
      {
        QDomElement value = contraint.firstChildElement( QStringLiteral( "DefaultValue" ) );
        if ( !value.isNull() && value.text() == QLatin1String( "TRUE" ) )
        {
          mCaps.supportsPaging = true;
          QgsDebugMsg( "Supports paging" );
        }
      }
      else if ( contraint.attribute( QStringLiteral( "name" ) ) == QLatin1String( "ImplementsStandardJoins" ) ||
                contraint.attribute( QStringLiteral( "name" ) ) == QLatin1String( "ImplementsSpatialJoins" ) /* WFS 2.0 */ )
      {
        QDomElement value = contraint.firstChildElement( QStringLiteral( "DefaultValue" ) );
        if ( !value.isNull() && value.text() == QLatin1String( "TRUE" ) )
        {
          mCaps.supportsJoins = true;
          QgsDebugMsg( "Supports joins" );
        }
      }
    }

    // In WFS 2.0, max features can also be set in Operation.GetFeature (e.g. GeoServer)
    // and we are also interested by resultType=hits for WFS 1.1
    QDomNodeList operationList = operationsMetadataElem.elementsByTagName( QStringLiteral( "Operation" ) );
    for ( int i = 0; i < operationList.size(); ++i )
    {
      QDomElement operation = operationList.at( i ).toElement();
      if ( operation.attribute( QStringLiteral( "name" ) ) == QLatin1String( "GetFeature" ) )
      {
        QDomNodeList operationContraintList = operation.elementsByTagName( QStringLiteral( "Constraint" ) );
        for ( int j = 0; j < operationContraintList.size(); ++j )
        {
          QDomElement contraint = operationContraintList.at( j ).toElement();
          if ( contraint.attribute( QStringLiteral( "name" ) ) == QLatin1String( "CountDefault" ) )
          {
            QDomElement value = contraint.firstChildElement( QStringLiteral( "DefaultValue" ) );
            if ( !value.isNull() )
            {
              mCaps.maxFeatures = value.text().toInt();
              QgsDebugMsg( QString( "maxFeatures: %1" ).arg( mCaps.maxFeatures ) );
            }
            break;
          }
        }

        QDomNodeList parameterList = operation.elementsByTagName( QStringLiteral( "Parameter" ) );
        for ( int j = 0; j < parameterList.size(); ++j )
        {
          QDomElement parameter = parameterList.at( j ).toElement();
          if ( parameter.attribute( QStringLiteral( "name" ) ) == QLatin1String( "resultType" ) )
          {
            QDomNodeList valueList = parameter.elementsByTagName( QStringLiteral( "Value" ) );
            for ( int k = 0; k < valueList.size(); ++k )
            {
              QDomElement value = valueList.at( k ).toElement();
              if ( value.text() == QLatin1String( "hits" ) )
              {
                mCaps.supportsHits = true;
                QgsDebugMsg( "Support hits" );
                break;
              }
            }
          }
        }

        break;
      }
    }
  }

  //go to <FeatureTypeList>
  QDomElement featureTypeListElem = doc.firstChildElement( QStringLiteral( "FeatureTypeList" ) );
  if ( featureTypeListElem.isNull() )
  {
    emit gotCapabilities();
    return;
  }

  // Parse operations supported for all feature types
  bool insertCap = false;
  bool updateCap = false;
  bool deleteCap = false;
  // WFS < 2
  if ( mCaps.version.startsWith( QLatin1String( "1" ) ) )
  {
    parseSupportedOperations( featureTypeListElem.firstChildElement( QStringLiteral( "Operations" ) ),
                              insertCap,
                              updateCap,
                              deleteCap );
  }
  else // WFS 2.0.0 tested on GeoServer
  {
    QDomNodeList operationNodes = doc.elementsByTagName( "Operation" );
    for ( int i = 0; i < operationNodes.count(); i++ )
    {
      QDomElement operationElement = operationNodes.at( i ).toElement( );
      if ( operationElement.isElement( ) && "Transaction" == operationElement.attribute( "name" ) )
      {
        insertCap = true;
        updateCap = true;
        deleteCap = true;
      }
    }
  }

  // get the <FeatureType> elements
  QDomNodeList featureTypeList = featureTypeListElem.elementsByTagName( QStringLiteral( "FeatureType" ) );
  for ( int i = 0; i < featureTypeList.size(); ++i )
  {
    FeatureType featureType;
    QDomElement featureTypeElem = featureTypeList.at( i ).toElement();

    //Name
    QDomNodeList nameList = featureTypeElem.elementsByTagName( QStringLiteral( "Name" ) );
    if ( nameList.length() > 0 )
    {
      featureType.name = nameList.at( 0 ).toElement().text();
    }
    //Title
    QDomNodeList titleList = featureTypeElem.elementsByTagName( QStringLiteral( "Title" ) );
    if ( titleList.length() > 0 )
    {
      featureType.title = titleList.at( 0 ).toElement().text();
    }
    //Abstract
    QDomNodeList abstractList = featureTypeElem.elementsByTagName( QStringLiteral( "Abstract" ) );
    if ( abstractList.length() > 0 )
    {
      featureType.abstract = abstractList.at( 0 ).toElement().text();
    }

    //DefaultSRS is always the first entry in the feature srs list
    QDomNodeList defaultCRSList = featureTypeElem.elementsByTagName( QStringLiteral( "DefaultSRS" ) );
    if ( defaultCRSList.length() == 0 )
      // In WFS 2.0, this is spelled DefaultCRS...
      defaultCRSList = featureTypeElem.elementsByTagName( QStringLiteral( "DefaultCRS" ) );
    if ( defaultCRSList.length() > 0 )
    {
      QString srsname( defaultCRSList.at( 0 ).toElement().text() );
      // Some servers like Geomedia advertize EPSG:XXXX even in WFS 1.1 or 2.0
      if ( srsname.startsWith( QLatin1String( "EPSG:" ) ) )
        mCaps.useEPSGColumnFormat = true;
      featureType.crslist.append( NormalizeSRSName( srsname ) );
    }

    //OtherSRS
    QDomNodeList otherCRSList = featureTypeElem.elementsByTagName( QStringLiteral( "OtherSRS" ) );
    if ( otherCRSList.length() == 0 )
      // In WFS 2.0, this is spelled OtherCRS...
      otherCRSList = featureTypeElem.elementsByTagName( QStringLiteral( "OtherCRS" ) );
    for ( int i = 0; i < otherCRSList.size(); ++i )
    {
      featureType.crslist.append( NormalizeSRSName( otherCRSList.at( i ).toElement().text() ) );
    }

    //Support <SRS> for compatibility with older versions
    QDomNodeList srsList = featureTypeElem.elementsByTagName( QStringLiteral( "SRS" ) );
    for ( int i = 0; i < srsList.size(); ++i )
    {
      featureType.crslist.append( NormalizeSRSName( srsList.at( i ).toElement().text() ) );
    }

    // Get BBox WFS 1.0 way
    QDomElement latLongBB = featureTypeElem.firstChildElement( QStringLiteral( "LatLongBoundingBox" ) );
    if ( latLongBB.hasAttributes() )
    {
      // Despite the name LatLongBoundingBox, the coordinates are supposed to
      // be expressed in <SRS>. From the WFS schema;
      // <!-- The LatLongBoundingBox element is used to indicate the edges of
      // an enclosing rectangle in the SRS of the associated feature type.
      featureType.bbox = QgsRectangle(
                           latLongBB.attribute( QStringLiteral( "minx" ) ).toDouble(),
                           latLongBB.attribute( QStringLiteral( "miny" ) ).toDouble(),
                           latLongBB.attribute( QStringLiteral( "maxx" ) ).toDouble(),
                           latLongBB.attribute( QStringLiteral( "maxy" ) ).toDouble() );
      featureType.bboxSRSIsWGS84 = false;

      // But some servers do not honour this and systematically reproject to WGS84
      // such as GeoServer. See http://osgeo-org.1560.x6.nabble.com/WFS-LatLongBoundingBox-td3813810.html
      // This is also true of TinyOWS
      if ( !featureType.crslist.isEmpty() &&
           featureType.bbox.xMinimum() >= -180 && featureType.bbox.yMinimum() >= -90 &&
           featureType.bbox.xMaximum() <= 180 && featureType.bbox.yMaximum() < 90 )
      {
        QgsCoordinateReferenceSystem crs = QgsCoordinateReferenceSystem::fromOgcWmsCrs( featureType.crslist[0] );
        if ( !crs.isGeographic() )
        {
          // If the CRS is projected then check that projecting the corner of the bbox, assumed to be in WGS84,
          // into the CRS, and then back to WGS84, works (check that we are in the validity area)
          QgsCoordinateReferenceSystem crsWGS84 = QgsCoordinateReferenceSystem::fromOgcWmsCrs( QStringLiteral( "CRS:84" ) );
          QgsCoordinateTransform ct( crsWGS84, crs );

          QgsPoint ptMin( featureType.bbox.xMinimum(), featureType.bbox.yMinimum() );
          QgsPoint ptMinBack( ct.transform( ct.transform( ptMin, QgsCoordinateTransform::ForwardTransform ), QgsCoordinateTransform::ReverseTransform ) );
          QgsPoint ptMax( featureType.bbox.xMaximum(), featureType.bbox.yMaximum() );
          QgsPoint ptMaxBack( ct.transform( ct.transform( ptMax, QgsCoordinateTransform::ForwardTransform ), QgsCoordinateTransform::ReverseTransform ) );

          QgsDebugMsg( featureType.bbox.toString() );
          QgsDebugMsg( ptMinBack.toString() );
          QgsDebugMsg( ptMaxBack.toString() );

          if ( fabs( featureType.bbox.xMinimum() - ptMinBack.x() ) < 1e-5 &&
               fabs( featureType.bbox.yMinimum() - ptMinBack.y() ) < 1e-5 &&
               fabs( featureType.bbox.xMaximum() - ptMaxBack.x() ) < 1e-5 &&
               fabs( featureType.bbox.yMaximum() - ptMaxBack.y() ) < 1e-5 )
          {
            QgsDebugMsg( "Values of LatLongBoundingBox are consistent with WGS84 long/lat bounds, so as the CRS is projected, assume they are indeed in WGS84 and not in the CRS units" );
            featureType.bboxSRSIsWGS84 = true;
          }
        }
      }
    }
    else
    {
      // WFS 1.1 way
      QDomElement WGS84BoundingBox = featureTypeElem.firstChildElement( QStringLiteral( "WGS84BoundingBox" ) );
      if ( !WGS84BoundingBox.isNull() )
      {
        QDomElement lowerCorner = WGS84BoundingBox.firstChildElement( QStringLiteral( "LowerCorner" ) );
        QDomElement upperCorner = WGS84BoundingBox.firstChildElement( QStringLiteral( "UpperCorner" ) );
        if ( !lowerCorner.isNull() && !upperCorner.isNull() )
        {
          QStringList lowerCornerList = lowerCorner.text().split( QStringLiteral( " " ), QString::SkipEmptyParts );
          QStringList upperCornerList = upperCorner.text().split( QStringLiteral( " " ), QString::SkipEmptyParts );
          if ( lowerCornerList.size() == 2 && upperCornerList.size() == 2 )
          {
            featureType.bbox = QgsRectangle(
                                 lowerCornerList[0].toDouble(),
                                 lowerCornerList[1].toDouble(),
                                 upperCornerList[0].toDouble(),
                                 upperCornerList[1].toDouble() );
            featureType.bboxSRSIsWGS84 = true;
          }
        }
      }
    }

    // Parse Operations specific to the type name
    parseSupportedOperations( featureTypeElem.firstChildElement( QStringLiteral( "Operations" ) ),
                              featureType.insertCap,
                              featureType.updateCap,
                              featureType.deleteCap );
    featureType.insertCap |= insertCap;
    featureType.updateCap |= updateCap;
    featureType.deleteCap |= deleteCap;

    mCaps.featureTypes.push_back( featureType );
  }

  Q_FOREACH ( const FeatureType& f, mCaps.featureTypes )
  {
    mCaps.setAllTypenames.insert( f.name );
    QString unprefixed( QgsWFSUtils::removeNamespacePrefix( f.name ) );
    if ( !mCaps.setAmbiguousUnprefixedTypename.contains( unprefixed ) )
    {
      if ( mCaps.mapUnprefixedTypenameToPrefixedTypename.contains( unprefixed ) )
      {
        mCaps.setAmbiguousUnprefixedTypename.insert( unprefixed );
        mCaps.mapUnprefixedTypenameToPrefixedTypename.remove( unprefixed );
      }
      else
      {
        mCaps.mapUnprefixedTypenameToPrefixedTypename[unprefixed] = f.name;
      }
    }
  }

  //go to <Filter_Capabilities>
  QDomElement filterCapabilitiesElem = doc.firstChildElement( QStringLiteral( "Filter_Capabilities" ) );
  if ( !filterCapabilitiesElem.isNull() )
    parseFilterCapabilities( filterCapabilitiesElem );

  // Hard-coded functions
  Function f_ST_GeometryFromText( QStringLiteral( "ST_GeometryFromText" ), 1, 2 );
  f_ST_GeometryFromText.returnType = QStringLiteral( "gml:AbstractGeometryType" );
  f_ST_GeometryFromText.argumentList << Argument( QStringLiteral( "wkt" ), QStringLiteral( "xs:string" ) );
  f_ST_GeometryFromText.argumentList << Argument( QStringLiteral( "srsname" ), QStringLiteral( "xs:string" ) );
  mCaps.functionList << f_ST_GeometryFromText;

  Function f_ST_GeomFromGML( QStringLiteral( "ST_GeomFromGML" ), 1 );
  f_ST_GeomFromGML.returnType = QStringLiteral( "gml:AbstractGeometryType" );
  f_ST_GeomFromGML.argumentList << Argument( QStringLiteral( "gml" ), QStringLiteral( "xs:string" ) );
  mCaps.functionList << f_ST_GeomFromGML;

  Function f_ST_MakeEnvelope( QStringLiteral( "ST_MakeEnvelope" ), 4, 5 );
  f_ST_MakeEnvelope.returnType = QStringLiteral( "gml:AbstractGeometryType" );
  f_ST_MakeEnvelope.argumentList << Argument( QStringLiteral( "minx" ), QStringLiteral( "xs:double" ) );
  f_ST_MakeEnvelope.argumentList << Argument( QStringLiteral( "miny" ), QStringLiteral( "xs:double" ) );
  f_ST_MakeEnvelope.argumentList << Argument( QStringLiteral( "maxx" ), QStringLiteral( "xs:double" ) );
  f_ST_MakeEnvelope.argumentList << Argument( QStringLiteral( "maxy" ), QStringLiteral( "xs:double" ) );
  f_ST_MakeEnvelope.argumentList << Argument( QStringLiteral( "srsname" ), QStringLiteral( "xs:string" ) );
  mCaps.functionList << f_ST_MakeEnvelope;

  emit gotCapabilities();
}

QString QgsWfsCapabilities::NormalizeSRSName( QString crsName )
{
  QRegExp re( "urn:ogc:def:crs:([^:]+).+([^:]+)", Qt::CaseInsensitive );
  if ( re.exactMatch( crsName ) )
  {
    return re.cap( 1 ) + ':' + re.cap( 2 );
  }
  // urn:x-ogc:def:crs:EPSG:xxxx as returned by http://maps.warwickshire.gov.uk/gs/ows? in WFS 1.1
  QRegExp re2( "urn:x-ogc:def:crs:([^:]+).+([^:]+)", Qt::CaseInsensitive );
  if ( re2.exactMatch( crsName ) )
  {
    return re2.cap( 1 ) + ':' + re2.cap( 2 );
  }
  return crsName;
}

int QgsWfsCapabilities::defaultExpirationInSec()
{
  QSettings s;
  return s.value( QStringLiteral( "/qgis/defaultCapabilitiesExpiry" ), "24" ).toInt() * 60 * 60;
}

void QgsWfsCapabilities::parseSupportedOperations( const QDomElement& operationsElem,
    bool& insertCap,
    bool& updateCap,
    bool& deleteCap )
{
  insertCap = false;
  updateCap = false;
  deleteCap = false;

  if ( operationsElem.isNull() )
  {
    return;
  }

  QDomNodeList childList = operationsElem.childNodes();
  for ( int i = 0; i < childList.size(); ++i )
  {
    QDomElement elt = childList.at( i ).toElement();
    QString elemName = elt.tagName();
    /* WFS 1.0 */
    if ( elemName == QLatin1String( "Insert" ) )
    {
      insertCap = true;
    }
    else if ( elemName == QLatin1String( "Update" ) )
    {
      updateCap = true;
    }
    else if ( elemName == QLatin1String( "Delete" ) )
    {
      deleteCap = true;
    }
    /* WFS 1.1 */
    else if ( elemName == QLatin1String( "Operation" ) )
    {
      QString elemText = elt.text();
      if ( elemText == QLatin1String( "Insert" ) )
      {
        insertCap = true;
      }
      else if ( elemText == QLatin1String( "Update" ) )
      {
        updateCap = true;
      }
      else if ( elemText == QLatin1String( "Delete" ) )
      {
        deleteCap = true;
      }
    }
  }
}

static QgsWfsCapabilities::Function getSpatialPredicate( const QString& name )
{
  QgsWfsCapabilities::Function f;
  // WFS 1.0 advertize Intersect, but for conveniency we internally convert it to Intersects
  if ( name == QLatin1String( "Intersect" ) )
    f.name = QStringLiteral( "ST_Intersects" );
  else
    f.name = ( name == QLatin1String( "BBOX" ) ) ? QStringLiteral( "BBOX" ) : "ST_" + name;
  f.returnType = QStringLiteral( "xs:boolean" );
  if ( name == QLatin1String( "DWithin" ) || name == QLatin1String( "Beyond" ) )
  {
    f.minArgs = 3;
    f.maxArgs = 3;
    f.argumentList << QgsWfsCapabilities::Argument( QStringLiteral( "geometry" ), QStringLiteral( "gml:AbstractGeometryType" ) );
    f.argumentList << QgsWfsCapabilities::Argument( QStringLiteral( "geometry" ), QStringLiteral( "gml:AbstractGeometryType" ) );
    f.argumentList << QgsWfsCapabilities::Argument( QStringLiteral( "distance" ) );
  }
  else
  {
    f.minArgs = 2;
    f.maxArgs = 2;
    f.argumentList << QgsWfsCapabilities::Argument( QStringLiteral( "geometry" ), QStringLiteral( "gml:AbstractGeometryType" ) );
    f.argumentList << QgsWfsCapabilities::Argument( QStringLiteral( "geometry" ), QStringLiteral( "gml:AbstractGeometryType" ) );
  }
  return f;
}

void QgsWfsCapabilities::parseFilterCapabilities( const QDomElement& filterCapabilitiesElem )
{
  // WFS 1.0
  QDomElement spatial_Operators = filterCapabilitiesElem.firstChildElement( QStringLiteral( "Spatial_Capabilities" ) ).firstChildElement( QStringLiteral( "Spatial_Operators" ) );
  QDomElement spatial_Operator = spatial_Operators.firstChildElement();
  while ( !spatial_Operator.isNull() )
  {
    QString name = spatial_Operator.tagName();
    if ( !name.isEmpty() )
    {
      mCaps.spatialPredicatesList << getSpatialPredicate( name );
    }
    spatial_Operator = spatial_Operator.nextSiblingElement();
  }

  // WFS 1.1 and 2.0
  QDomElement spatialOperators = filterCapabilitiesElem.firstChildElement( QStringLiteral( "Spatial_Capabilities" ) ).firstChildElement( QStringLiteral( "SpatialOperators" ) );
  QDomElement spatialOperator = spatialOperators.firstChildElement( QStringLiteral( "SpatialOperator" ) );
  while ( !spatialOperator.isNull() )
  {
    QString name = spatialOperator.attribute( QStringLiteral( "name" ) );
    if ( !name.isEmpty() )
    {
      mCaps.spatialPredicatesList << getSpatialPredicate( name );
    }
    spatialOperator = spatialOperator.nextSiblingElement( QStringLiteral( "SpatialOperator" ) );
  }

  // WFS 1.0
  QDomElement function_Names = filterCapabilitiesElem.firstChildElement( QStringLiteral( "Scalar_Capabilities" ) )
                               .firstChildElement( QStringLiteral( "Arithmetic_Operators" ) )
                               .firstChildElement( QStringLiteral( "Functions" ) )
                               .firstChildElement( QStringLiteral( "Function_Names" ) );
  QDomElement function_NameElem = function_Names.firstChildElement( QStringLiteral( "Function_Name" ) );
  while ( !function_NameElem.isNull() )
  {
    Function f;
    f.name = function_NameElem.text();
    bool ok;
    int nArgs = function_NameElem.attribute( QStringLiteral( "nArgs" ) ).toInt( &ok );
    if ( ok )
    {
      if ( nArgs >= 0 )
      {
        f.minArgs = nArgs;
        f.maxArgs = nArgs;
      }
      else
      {
        f.minArgs = -nArgs;
      }
    }
    mCaps.functionList << f;
    function_NameElem = function_NameElem.nextSiblingElement( QStringLiteral( "Function_Name" ) );
  }

  // WFS 1.1
  QDomElement functionNames = filterCapabilitiesElem.firstChildElement( QStringLiteral( "Scalar_Capabilities" ) )
                              .firstChildElement( QStringLiteral( "ArithmeticOperators" ) )
                              .firstChildElement( QStringLiteral( "Functions" ) )
                              .firstChildElement( QStringLiteral( "FunctionNames" ) );
  QDomElement functionNameElem = functionNames.firstChildElement( QStringLiteral( "FunctionName" ) );
  while ( !functionNameElem.isNull() )
  {
    Function f;
    f.name = functionNameElem.text();
    bool ok;
    int nArgs = functionNameElem.attribute( QStringLiteral( "nArgs" ) ).toInt( &ok );
    if ( ok )
    {
      if ( nArgs >= 0 )
      {
        f.minArgs = nArgs;
        f.maxArgs = nArgs;
      }
      else
      {
        f.minArgs = -nArgs;
      }
    }
    mCaps.functionList << f;
    functionNameElem = functionNameElem.nextSiblingElement( QStringLiteral( "FunctionName" ) );
  }

  QDomElement functions = filterCapabilitiesElem.firstChildElement( QStringLiteral( "Functions" ) );
  QDomElement functionElem = functions.firstChildElement( QStringLiteral( "Function" ) );
  while ( !functionElem.isNull() )
  {
    QString name = functionElem.attribute( QStringLiteral( "name" ) );
    if ( !name.isEmpty() )
    {
      Function f;
      f.name = name;
      QDomElement returnsElem = functionElem.firstChildElement( QStringLiteral( "Returns" ) );
      f.returnType = returnsElem.text();
      QDomElement argumentsElem = functionElem.firstChildElement( QStringLiteral( "Arguments" ) );
      QDomElement argumentElem = argumentsElem.firstChildElement( QStringLiteral( "Argument" ) );
      while ( !argumentElem.isNull() )
      {
        Argument arg;
        arg.name = argumentElem.attribute( QStringLiteral( "name" ) );
        arg.type = argumentElem.firstChildElement( QStringLiteral( "Type" ) ).text();
        f.argumentList << arg;
        argumentElem = argumentElem.nextSiblingElement( QStringLiteral( "Argument" ) );
      }
      f.minArgs = f.argumentList.count();
      f.maxArgs = f.argumentList.count();
      mCaps.functionList << f;
    }
    functionElem = functionElem.nextSiblingElement( QStringLiteral( "Function" ) );
  }
}

QString QgsWfsCapabilities::errorMessageWithReason( const QString& reason )
{
  return tr( "Download of capabilities failed: %1" ).arg( reason );
}
