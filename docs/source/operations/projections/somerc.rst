.. _somerc:

********************************************************************************
Swiss Oblique Mercator
********************************************************************************

.. figure:: ./images/somerc.png
   :width: 500 px
   :align: center
   :alt:   Swiss Oblique Mercator

   proj-string: ``+proj=somerc``


Parameters
################################################################################

.. note:: All parameters are optional for the projection.

.. option:: +lat_0=<value>

    Ellipsoidal latitude of the normal parallel, in degrees. This determines
    the conformal mapping from the ellipsoid to the intermediate sphere.
    Defaults to 0.

.. option:: +latc=<value>

    Optional projection-centre latitude **on the intermediate sphere**, in
    degrees, between -90 and 90 inclusive. This is a local extension, not an
    upstream PROJ 9.6 parameter. It controls the sphere-to-cylinder rotation
    independently of the normal parallel. It does not change the intermediate
    sphere's radius or the ellipsoid-to-sphere mapping.

    If omitted, the centre remains the spherical image of the normal parallel,
    preserving the ordinary Swiss Oblique Mercator calculation. An explicit
    zero selects the spherical equator; it does not request the default.

    For Hungarian EOV, the independent latitudes are ``+lat_0=47.1666666666667``
    and ``+latc=47.1``. See Toth and Hajdu, `GISopen 2022, slides 13-16
    <https://www.gisopen.hu/2022/ppt/H3.pdf>`_. This parameter extends the
    executable projection only; it does not register a new EPSG/WKT method.

.. include:: ../options/lon_0.rst

.. include:: ../options/ellps.rst

.. include:: ../options/R.rst

.. include:: ../options/k_0.rst

.. include:: ../options/x_0.rst

.. include:: ../options/y_0.rst
