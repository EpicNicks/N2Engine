#pragma once

#include <cstdint>

namespace N2Engine::Editor
{
    /**
     * The bookkeeping behind render on demand (#79, E7a): which picture the viewport would show now, and whether the
     * last one rendered is still it.
     *
     * The picture depends on the scene, the editor camera, the viewport size, the assets and the project's settings.
     * Each of those changing is a MarkChanged(), which moves the revision on; a frame rendered at a revision carries
     * it, and a client that holds the frame of the current revision needs no other. Revisions are never 0 (a client
     * passes 0 for "I have no frame"), they only grow, and after 4294967295 they wrap to 1.
     *
     * Not thread-safe: the editor server's main thread owns it.
     */
    class FrameTracker
    {
    public:
        /// `firstRevision` is where the numbering starts (0 is made 1). The server starts it at its event epoch, a
        /// random number, so a client that reconnects to another host (or to this one after a restart) holding the
        /// frame of revision N isn't told "not modified" by a host whose revision happens to be N too.
        explicit FrameTracker(const uint32_t firstRevision = 1) : _revision(firstRevision == 0 ? 1 : firstRevision) {}

        /// The revision of the picture the viewport shows now
        [[nodiscard]] uint32_t Revision() const { return _revision; }

        /**
         * The picture changed: moves the revision on and drops the cached frame's claim to be current.
         * @returns true for the first change since the last frame was rendered, so the caller tells clients once
         *          ("frameChanged") rather than for every step of a drag; a render re-arms it
         */
        bool MarkChanged()
        {
            ++_revision;
            if (_revision == 0)
            {
                _revision = 1;
            }
            const bool announce = !_announced;
            _announced = true;
            return announce;
        }

        /**
         * Notes the scene as it is now, and calls MarkChanged() when it is not the one last seen: another scene
         * object, or another scene revision. This catches a scene change no handler reported (a belt for the explicit
         * MarkChanged calls). The first observation only records the scene: nothing had been rendered before it.
         * @returns true when it marked a change that needs announcing (see MarkChanged)
         */
        bool ObserveScene(const void *scene, const uint32_t sceneRevision)
        {
            const bool known = _sceneObserved;
            const bool same = known && scene == _scene && sceneRevision == _sceneRevision;
            _sceneObserved = true;
            _scene = scene;
            _sceneRevision = sceneRevision;
            return known && !same ? MarkChanged() : false;
        }

        /// True when a client that holds the frame of `sinceRevision` has the current picture: this server rendered a
        /// frame at the current revision, and that is the revision the client holds. A client with no frame passes 0,
        /// which is never current, so it always gets one.
        [[nodiscard]] bool IsCurrent(const uint32_t sinceRevision) const
        {
            return sinceRevision != 0 && sinceRevision == _revision && _renderedRevision == _revision;
        }

        /// True when the frame held in the server's buffer is the current picture (no render needed to answer)
        [[nodiscard]] bool HasCurrentFrame() const { return _frameValid && _renderedRevision == _revision; }

        /// Records the scene as it is now without calling it a change: for a caller that has just reported the change
        /// itself (MarkChanged), so ObserveScene doesn't count it a second time
        void RecordScene(const void *scene, const uint32_t sceneRevision)
        {
            _sceneObserved = true;
            _scene = scene;
            _sceneRevision = sceneRevision;
        }

        /// A frame couldn't be rendered: the change that was announced went unanswered, so the next change is announced
        /// again rather than waiting for a render that failed
        void RearmAnnouncement() { _announced = false; }

        /// A frame of the current revision is now in the buffer: it is served until the picture changes
        void MarkRendered()
        {
            _renderedRevision = _revision;
            _frameValid = true;
            _announced = false;
        }

        /// The buffer no longer holds a frame of the editor view (another command used it: RenderFrame draws the
        /// game camera's picture into the same buffer). The picture hasn't changed, so clients' frames stay current.
        void InvalidateBuffer() { _frameValid = false; }

    private:
        uint32_t _revision;
        // The revision of the last frame rendered (0: none yet); a revision is never 0
        uint32_t _renderedRevision = 0;
        bool _frameValid = false;
        // A change has been announced and no frame rendered since
        bool _announced = false;

        bool _sceneObserved = false;
        const void *_scene = nullptr;
        uint32_t _sceneRevision = 0;
    };
}
