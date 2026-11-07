import QtQuick
import QtQuick.Pdf

// Kept in its own file so the app still runs where QtPdf (qt6-webengine) is
// missing; the preview then falls back to the file details.
PdfMultiPageView {
    id: view
    property url source
    document: PdfDocument { source: view.source }
}
